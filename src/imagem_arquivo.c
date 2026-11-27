/* hdclone - gravacao/leitura da imagem em partes, gravacao direta */

/* ------------------------------------------------------------------ */
/* arquivos de imagem (com divisao em partes)                          */
/* ------------------------------------------------------------------ */

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    char base[600];
    int part;
    u64 cur;   /* bytes na parte atual */
    u64 limit; /* tamanho maximo de cada parte (0 = sem limite) */
    u64 total;
    u64 pos;   /* leitura: posicao na parte atual */
    u64 psize; /* leitura: tamanho da parte atual */
    /* gravacao: buffer de 4 MB; com "direct" grava sem passar pelo cache do
       sistema (FILE_FLAG_NO_BUFFERING / O_DIRECT), o que evita que o Windows XP
       com pouca memoria fique lento ao gravar arquivos enormes */
    u8 *wbuf;
    u32 wlen;
    int direct;
    int padded;   /* ultima parte gravada com enchimento: precisa truncar */
    u64 truelen;  /* tamanho real da ultima parte */
    char err[300];
} Img;

#define WBUF (4u * 1024 * 1024)
#define WALIGN 4096u

static void part_name(const char *base, int part, char *out)
{
    if (part <= 1) strcpy(out, base);
    else sprintf(out, "%s.%03d", base, part);
}

static int img_open_part(Img *m, int write)
{
    char nm[620];
    part_name(m->base, m->part, nm);
#ifdef _WIN32
    m->h = CreateFileA(nm, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL,
                       write ? CREATE_ALWAYS : OPEN_EXISTING,
                       (write && m->direct) ? FILE_FLAG_NO_BUFFERING : FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (m->h == INVALID_HANDLE_VALUE && write && m->direct) { /* sem suporte: grava pelo cache */
        m->direct = 0;
        m->h = CreateFileA(nm, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
    if (m->h == INVALID_HANDLE_VALUE) return -1;
    res_add(RES_HANDLE, m->h);
    m->pos = 0;
    {
        LARGE_INTEGER sz;
        m->psize = GetFileSizeEx(m->h, &sz) ? (u64)sz.QuadPart : 0;
    }
    return 0;
#else
    struct stat st;
#ifdef O_DIRECT
    if (write && m->direct) {
        m->fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
        if (m->fd < 0) m->direct = 0; /* ex.: tmpfs nao aceita O_DIRECT */
    } else
#endif
        m->fd = -1;
    if (m->fd < 0) m->fd = open(nm, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0644);
    if (m->fd < 0) return -1;
    res_add(RES_HANDLE, (void *)(intptr_t)(m->fd + 1));
    m->pos = 0;
    m->psize = fstat(m->fd, &st) == 0 ? (u64)st.st_size : 0;
    return 0;
#endif
}

static void img_close_part(Img *m)
{
#ifdef _WIN32
    if (m->h && m->h != INVALID_HANDLE_VALUE) { res_del(m->h); CloseHandle(m->h); }
    m->h = NULL;
#else
    if (m->fd > 0) { res_del((void *)(intptr_t)(m->fd + 1)); close(m->fd); }
    m->fd = -1;
#endif
}

/* gravacao direta possivel? (setor do volume de destino divide 4096) */
static int direct_ok(const char *path)
{
#ifdef _WIN32
    char full[MAX_PATH], root[MAX_PATH];
    DWORD spc, bps, fc, tc;
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL) || !GetVolumePathNameA(full, root, MAX_PATH)) return 0;
    if (!GetDiskFreeSpaceA(root, &spc, &bps, &fc, &tc) || !bps || WALIGN % bps) return 0;
    return 1;
#else
    (void)path;
    return getenv("HDCLONE_NO_DIRECT") ? 0 : 1;
#endif
}

static int img_open(Img *m, const char *path, int write, u64 limit)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->base, path, sizeof(m->base) - 1);
    m->part = 1;
    m->limit = limit;
    if (write) {
        m->direct = direct_ok(path);
        m->wbuf = xalloc(WBUF + WALIGN);
    }
    return img_open_part(m, write);
}

static int raw_write(Img *m, const void *p, u32 n)
{
#ifdef _WIN32
    DWORD w;
    return (WriteFile(m->h, p, n, &w, NULL) && w == n) ? 0 : -1;
#else
    const u8 *q = p;
    while (n) {
        ssize_t r = write(m->fd, q, n);
        if (r <= 0) return -1;
        q += r; n -= (u32)r;
    }
    return 0;
#endif
}

static int raw_read(Img *m, void *p, u32 n)
{
#ifdef _WIN32
    DWORD r;
    if (!ReadFile(m->h, p, n, &r, NULL)) return -1;
    return (int)r;
#else
    ssize_t r = read(m->fd, p, n);
    return (int)r;
#endif
}

/* esvazia o buffer de gravacao nas partes (final = ultimo pedaco, pode nao ser alinhado) */
static int img_flush(Img *m, int final)
{
    u32 off = 0;
    while (off < m->wlen) {
        u32 piece = m->wlen - off, wr;
        if (m->limit && m->cur >= m->limit) {
            img_close_part(m);
            m->part++;
            m->cur = 0;
            if (img_open_part(m, 1) != 0) {
                snprintf(m->err, sizeof(m->err), "nao foi possivel criar a proxima parte da imagem");
                return -1;
            }
        }
        if (m->limit && piece > m->limit - m->cur) piece = (u32)(m->limit - m->cur);
        wr = piece;
        if (m->direct && (wr % WALIGN)) { /* so no fim: completa com zeros e depois trunca */
            u32 pad = WALIGN - wr % WALIGN;
            if (!final) { snprintf(m->err, sizeof(m->err), "erro interno de alinhamento"); return -1; }
            memset(m->wbuf + off + wr, 0, pad);
            wr += pad;
            m->padded = 1;
            m->truelen = m->cur + piece;
        }
        if (raw_write(m, m->wbuf + off, wr) != 0) {
            snprintf(m->err, sizeof(m->err), "falha ao gravar imagem (disco de destino cheio?)");
            return -1;
        }
        m->cur += piece;
        off += piece;
    }
    m->wlen = 0;
    return 0;
}

/* acrescenta bytes a imagem; -1 em erro (mensagem em m->err). Seguro em thread. */
static int img_write_nd(Img *m, const void *buf, u64 n)
{
    const u8 *p = buf;
    while (n) {
        u32 take = WBUF - m->wlen;
        if (take > n) take = (u32)n;
        memcpy(m->wbuf + m->wlen, p, take);
        m->wlen += take;
        p += take; n -= take; m->total += take;
        if (m->wlen == WBUF && img_flush(m, 0) != 0) return -1;
    }
    return 0;
}

static void img_write(Img *m, const void *buf, u64 n)
{
    if (img_write_nd(m, buf, n) != 0) die(m->err);
}

/* termina a gravacao: grava o resto, fecha e acerta o tamanho da ultima parte */
static void img_finish(Img *m)
{
    char nm[620];
    if (img_flush(m, 1) != 0) die(m->err);
    img_close_part(m);
    xfree(m->wbuf);
    m->wbuf = NULL;
    if (!m->padded) return;
    part_name(m->base, m->part, nm);
#ifdef _WIN32
    {
        HANDLE h = CreateFileA(nm, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER li;
        int ok = 0;
        li.QuadPart = (LONGLONG)m->truelen;
        if (h != INVALID_HANDLE_VALUE) {
            ok = SetFilePointerEx(h, li, NULL, FILE_BEGIN) && SetEndOfFile(h);
            CloseHandle(h);
        }
        if (!ok) die("falha ao finalizar o arquivo de imagem");
    }
#else
    if (truncate(nm, (off_t)m->truelen) != 0) die("falha ao finalizar o arquivo de imagem");
#endif
}

/* retorna 0 se leu tudo, -1 se acabou o arquivo/partes */
static int img_read(Img *m, void *buf, u64 n)
{
    u8 *p = buf;
    while (n) {
        u32 want = n > 0x40000000 ? 0x40000000 : (u32)n;
        int r = raw_read(m, p, want);
        if (r < 0) return -1;
        if (r == 0) {
            img_close_part(m);
            m->part++;
            if (img_open_part(m, 0) != 0) return -1;
            continue;
        }
        p += r; n -= (u64)r; m->total += (u64)r; m->pos += (u64)r;
    }
    return 0;
}

/* posiciona a leitura em (parte, posicao) */
static int img_seek(Img *m, int part, u64 pos)
{
    if (m->part != part) {
        img_close_part(m);
        m->part = part;
        if (img_open_part(m, 0) != 0) return -1;
    }
#ifdef _WIN32
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)pos;
        if (!SetFilePointerEx(m->h, li, NULL, FILE_BEGIN)) return -1;
    }
#else
    if (lseek(m->fd, (off_t)pos, SEEK_SET) < 0) return -1;
#endif
    m->pos = pos;
    return 0;
}

/* pula n bytes sem ler (atravessa partes) */
static int img_skip(Img *m, u64 n)
{
    while (n) {
        u64 rem = m->psize > m->pos ? m->psize - m->pos : 0;
        if (n <= rem) return img_seek(m, m->part, m->pos + n);
        n -= rem;
        if (img_seek(m, m->part + 1, 0) != 0) return -1;
    }
    return 0;
}
