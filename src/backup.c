/* hdclone - progresso, threads/pipeline, espaco, retomada, indice, BACKUP */

/* ------------------------------------------------------------------ */
/* progresso                                                           */
/* ------------------------------------------------------------------ */

static u64 g_t0, g_tlast;

static void progress(u64 done, u64 total, int force)
{
    u64 t = now_ms(), el;
    double pct, speed;
    unsigned long eta;
    if (g_quiet || (!force && t - g_tlast < 500)) return;
    g_tlast = t;
    el = t - g_t0;
    if (!el) el = 1;
    pct = total ? 100.0 * (double)done / (double)total : 100.0;
    speed = ((double)done / 1048576.0) / ((double)el / 1000.0);
    eta = (speed > 0 && done < total) ? (unsigned long)(((double)(total - done) / 1048576.0) / speed) : 0;
#ifdef HDCLONE_GUI
    (void)pct;
    ui_progress(done, total, speed, eta);
    return;
#endif
    printf("\r  %5.1f%%  %llu / %llu MB  %6.1f MB/s  restante %02lu:%02lu:%02lu ",
           pct, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20), speed,
           eta / 3600, (eta / 60) % 60, eta % 60);
    fflush(stdout);
}

static int confirm(const char *msg)
{
    char line[64];
#ifdef HDCLONE_GUI
    (void)msg; (void)line;
    return 1; /* a interface grafica ja confirmou antes de iniciar */
#endif
    printf("%s\nDigite SIM para continuar: ", msg);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    return strncmp(line, "SIM", 3) == 0 || strncmp(line, "sim", 3) == 0;
}

/* ------------------------------------------------------------------ */
/* BACKUP                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* threads (so recursos que existem no Windows XP) e pipeline          */
/* ------------------------------------------------------------------ */
/*
 * Ler, comprimir e gravar acontecem ao mesmo tempo:
 *   produtor (le o disco/imagem) -> N trabalhadores (comprimem) -> consumidor (grava em ordem)
 * Mesmo com 1 nucleo isso ajuda: enquanto o processador comprime, o disco ja le o
 * proximo bloco. As threads nunca chamam die(): registram o erro e param; quem
 * chama pipe_run() encerra tudo em ordem e so entao reporta.
 */

#ifdef _WIN32
typedef HANDLE xsem_t;
typedef CRITICAL_SECTION xmtx_t;
typedef HANDLE xthr_t;
static void xsem_init(xsem_t *x, int v) { *x = CreateSemaphoreA(NULL, v, 0x7FFFFFFF, NULL); }
static void xsem_wait(xsem_t *x) { WaitForSingleObject(*x, INFINITE); }
static void xsem_post(xsem_t *x, int n) { ReleaseSemaphore(*x, n, NULL); }
static void xsem_free(xsem_t *x) { CloseHandle(*x); }
static void xmtx_init(xmtx_t *x) { InitializeCriticalSection(x); }
static void xmtx_lock(xmtx_t *x) { EnterCriticalSection(x); }
static void xmtx_unlock(xmtx_t *x) { LeaveCriticalSection(x); }
static void xmtx_free(xmtx_t *x) { DeleteCriticalSection(x); }
static void xsleep(int ms) { Sleep(ms); }
#else
typedef sem_t xsem_t;
typedef pthread_mutex_t xmtx_t;
typedef pthread_t xthr_t;
static void xsem_init(xsem_t *x, int v) { sem_init(x, 0, (unsigned)v); }
static void xsem_wait(xsem_t *x) { while (sem_wait(x) != 0 && errno == EINTR) ; }
static void xsem_post(xsem_t *x, int n) { while (n-- > 0) sem_post(x); }
static void xsem_free(xsem_t *x) { sem_destroy(x); }
static void xmtx_init(xmtx_t *x) { pthread_mutex_init(x, NULL); }
static void xmtx_lock(xmtx_t *x) { pthread_mutex_lock(x); }
static void xmtx_unlock(xmtx_t *x) { pthread_mutex_unlock(x); }
static void xmtx_free(xmtx_t *x) { pthread_mutex_destroy(x); }
static void xsleep(int ms) { usleep((useconds_t)ms * 1000); }
#endif

typedef struct {
    void (*fn)(void *);
    void *arg;
} ThrArg;

#ifdef _WIN32
static DWORD WINAPI thr_tramp(LPVOID a) { ((ThrArg *)a)->fn(((ThrArg *)a)->arg); return 0; }
static int xthr_start(xthr_t *t, ThrArg *a)
{
    DWORD id;
    *t = CreateThread(NULL, 0, thr_tramp, a, 0, &id);
    return *t ? 0 : -1;
}
static void xthr_join(xthr_t *t) { WaitForSingleObject(*t, INFINITE); CloseHandle(*t); }
#else
static void *thr_tramp(void *a) { ((ThrArg *)a)->fn(((ThrArg *)a)->arg); return NULL; }
static int xthr_start(xthr_t *t, ThrArg *a) { return pthread_create(t, NULL, thr_tramp, a); }
static void xthr_join(xthr_t *t) { pthread_join(*t, NULL); }
#endif

static int ncores(void)
{
    int n;
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    n = (int)si.dwNumberOfProcessors;
#else
    n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (getenv("HDCLONE_THREADS")) n = atoi(getenv("HDCLONE_THREADS"));
    return n < 1 ? 1 : n > 8 ? 8 : n;
}

#define CBUF_SIZE (CHUNK_SIZE + CHUNK_SIZE / 8 + 4096) /* >= compressBound e ZSTD_compressBound de 1 MB */
#define T_PENDING 0xFF  /* bloco lido, aguardando compressao */

typedef struct {
    u64 idx;
    u32 len, stored, crc;
    u8 type;
    u8 *buf, *cbuf;
    const u8 *out; /* restauracao: dados a gravar (NULL = pular) */
} Slot;

typedef struct Pipe Pipe;
struct Pipe {
    int K, N;
    Slot *s;
    u64 total;          /* numero de blocos */
    u64 first;          /* primeiro bloco (retomada de backup interrompido) */
    int (*produce)(Pipe *, Slot *);
    int (*work)(Pipe *, Slot *, int w);
    int (*consume)(Pipe *, Slot *);
    void *ctx;
    volatile int abort, cons_done;
    char err[600];
    xmtx_t mtx;
    u64 next_work;
    volatile u64 done_bytes, total_bytes;
    xsem_t sfree, swork, *sdone;
};

static void pipe_wake(Pipe *p)
{
    int k;
    xsem_post(&p->sfree, p->K);
    xsem_post(&p->swork, p->N + 1);
    for (k = 0; k < p->K; k++) xsem_post(&p->sdone[k], 1);
}

/* registra o primeiro erro e faz todas as threads pararem */
static void pipe_fail(Pipe *p, const char *msg)
{
    int first = 0;
    xmtx_lock(&p->mtx);
    if (!p->abort) {
        snprintf(p->err, sizeof(p->err), "%s", msg);
        p->abort = 1;
        first = 1;
    }
    xmtx_unlock(&p->mtx);
    if (first) pipe_wake(p);
}

static void pipe_producer(void *a)
{
    Pipe *p = a;
    u64 i;
    for (i = 0; i < p->total; i++) {
        Slot *sl;
        xsem_wait(&p->sfree);
        if (p->abort) return;
        sl = &p->s[i % p->K];
        sl->idx = p->first + i;
        if (p->produce(p, sl) != 0) return;
        xsem_post(&p->swork, 1);
    }
}

typedef struct {
    Pipe *p;
    int w;
    ThrArg ta;
} WArg;

static void pipe_worker(void *a)
{
    WArg *wa = a;
    Pipe *p = wa->p;
    for (;;) {
        u64 i;
        xsem_wait(&p->swork);
        if (p->abort) return;
        xmtx_lock(&p->mtx);
        i = p->next_work++;
        xmtx_unlock(&p->mtx);
        if (i >= p->total) return;
        if (p->work(p, &p->s[i % p->K], wa->w) != 0) return;
        xsem_post(&p->sdone[i % p->K], 1);
    }
}

static void pipe_consumer(void *a)
{
    Pipe *p = a;
    u64 i;
    for (i = 0; i < p->total; i++) {
        xsem_wait(&p->sdone[i % p->K]);
        if (p->abort) break;
        if (p->consume(p, &p->s[i % p->K]) != 0) break;
        xsem_post(&p->sfree, 1);
    }
    p->cons_done = 1;
}

/* aloca os blocos de trabalho (K = 2N+2) */
static void pipe_init(Pipe *p, u64 total, u64 total_bytes)
{
    int k;
    memset(p, 0, sizeof(*p));
    p->N = ncores();
    p->K = 2 * p->N + 2;
    p->total = total;
    p->total_bytes = total_bytes;
    p->s = xalloc(sizeof(Slot) * p->K);
    for (k = 0; k < p->K; k++) {
        p->s[k].buf = xalloc(CHUNK_SIZE);
        p->s[k].cbuf = xalloc(CBUF_SIZE);
    }
}

static void pipe_free(Pipe *p)
{
    int k;
    for (k = 0; k < p->K; k++) {
        xfree(p->s[k].buf);
        xfree(p->s[k].cbuf);
    }
    xfree(p->s);
}

/* roda o pipeline; mostra progresso e atende ao cancelamento. 0 = ok */
static int pipe_run(Pipe *p)
{
    xthr_t tp, tc, tw[8];
    ThrArg ap, ac;
    WArg wa[8];
    int k, nw = 0;
    xmtx_init(&p->mtx);
    xsem_init(&p->sfree, p->K);
    xsem_init(&p->swork, 0);
    p->sdone = malloc(sizeof(xsem_t) * p->K);
    for (k = 0; k < p->K; k++) xsem_init(&p->sdone[k], 0);
    ap.fn = pipe_producer; ap.arg = p;
    ac.fn = pipe_consumer; ac.arg = p;
    g_t0 = g_tlast = now_ms();
    if (xthr_start(&tc, &ac) != 0) die("nao foi possivel criar threads");
    for (k = 0; k < p->N; k++) {
        wa[k].p = p;
        wa[k].w = k;
        wa[k].ta.fn = pipe_worker;
        wa[k].ta.arg = &wa[k];
        if (xthr_start(&tw[k], &wa[k].ta) == 0) nw++;
        else break;
    }
    if (!nw || xthr_start(&tp, &ap) != 0) {
        pipe_fail(p, "nao foi possivel criar threads");
        p->cons_done = 1;
    }
    while (!p->cons_done) {
        xsleep(100);
        if (g_cancel) pipe_fail(p, "Operacao cancelada pelo usuario.");
        progress(p->done_bytes, p->total_bytes, 0);
    }
    if (!p->abort) xsem_post(&p->swork, p->N + 1); /* libera os trabalhadores */
    if (nw) xthr_join(&tp);
    for (k = 0; k < nw; k++) xthr_join(&tw[k]);
    xthr_join(&tc);
    if (!p->abort) progress(p->total_bytes, p->total_bytes, 1);
    xsem_free(&p->sfree);
    xsem_free(&p->swork);
    for (k = 0; k < p->K; k++) xsem_free(&p->sdone[k]);
    free(p->sdone);
    xmtx_free(&p->mtx);
    return p->abort ? -1 : 0;
}

typedef struct {
    int level;    /* zstd: 1..19 (padrao 3); --compat: zlib 1..9 (padrao 6) */
    int compat;   /* formato antigo (zlib), legivel por versoes anteriores */
    u64 split;
    int raw;
    int yes;
    int force;
    int zero_free;
    int no_vss;
    int keep_sys;
    int verify;   /* verificar a imagem (ou o disco clonado) ao terminar */
    int rescue_all; /* setores defeituosos: tentar todos, um a um (lento) */
    int skip_restore; /* pontos de restauracao e despejos de memoria gravados como zeros */
    int resume;   /* continuar um backup interrompido */
    int hwfix;    /* preparar o Windows restaurado/clonado para outro hardware */
} Opts;

static int g_vss_used; /* ultimo backup usou VSS */
static int g_last_direct; /* ultimo backup gravou sem passar pelo cache */
static u64 g_last_rfails;
static void cmd_verify(const char *path);
static int g_shrunk;   /* ultima restauracao reduziu uma particao */

/* ---- backup em pipeline ---- */

typedef struct {
    Dev *d;
    Img *m;
    Opts *o;
    u64 bad, used, nfree;
    ZSTD_CCtx *zc[8];
    u8 *probe[8];
    u8 *idx;    /* cabecalhos de todos os blocos (indice gravado no fim da imagem) */
    u64 unread;   /* bytes nao recuperados (setores defeituosos ou area pulada) */
    u64 rfails;   /* leituras que falharam */
    int bad_streak; /* blocos seguidos totalmente ilegiveis ("zona ruim") */
} BkCtx;

/* ---- leitura de bloco com setores defeituosos ----
   Num HD com defeito cada leitura que falha pode levar varios segundos. Em vez de
   tentar os 2048 setores do bloco um a um, divide em largura: 64 KB -> 4 KB -> setor,
   lendo sempre primeiro as partes boas, com um limite de falhas por bloco. Numa
   "zona ruim" (bloco anterior todo ilegivel) testa 4 pontos antes de insistir.
   --rescue-all mantem o metodo antigo (todos os setores, um a um). */
#define RESCUE_BUDGET 32

static void rescue_read(Dev *d, u8 *buf, u32 len, u64 off, u64 *unread, u64 *fails, int *streak, int all)
{
    u32 piece[2048], plen[2048], nf = 0, i, j, steps[3];
    int budget = RESCUE_BUDGET, lvl;
    u32 ss = d->sector;
    if (all) { /* metodo antigo: setor por setor */
        for (i = 0; i < len; i += ss)
            if (dev_pread(d, buf + i, ss, off + i) != 0) {
                memset(buf + i, 0, ss);
                *unread += ss;
                (*fails)++;
            }
        return;
    }
    if (*streak > 0) { /* zona ruim: 4 amostras de 64 KB; se todas falharem, pula o bloco */
        int ok = 0;
        for (i = 0; i < 4 && !ok; i++) {
            u32 o = i * (len / 4), n = len - o < 65536 ? len - o : 65536;
            n -= n % ss;
            if (n && dev_pread(d, buf + o, n, off + o) == 0) ok = 1;
            else (*fails)++;
        }
        if (!ok) {
            memset(buf, 0, len);
            *unread += len;
            (*streak)++;
            return;
        }
    }
    steps[0] = 65536; steps[1] = ss; steps[2] = ss; /* 64 KB -> setor */
    /* nivel 0: pedacos de 64 KB do bloco inteiro */
    piece[0] = 0;
    plen[0] = len;
    nf = 1;
    for (lvl = 0; lvl < 3 && nf; lvl++) {
        u32 np = 0, st = steps[lvl] < ss ? ss : steps[lvl];
        u32 npiece[2048], nplen[2048];
        for (i = 0; i < nf; i++) {
            for (j = 0; j < plen[i]; j += st) {
                u32 o = piece[i] + j, n = plen[i] - j < st ? plen[i] - j : st;
                if (budget <= 0 || dev_pread(d, buf + o, n, off + o) != 0) {
                    if (budget > 0) { budget--; (*fails)++; }
                    if (np < 2048) { npiece[np] = o; nplen[np++] = n; }
                }
            }
        }
        if (st <= ss || budget <= 0) { /* nao da para dividir mais (ou acabou o limite) */
            for (i = 0; i < np; i++) {
                memset(buf + npiece[i], 0, nplen[i]);
                *unread += nplen[i];
            }
            np = 0;
        }
        memcpy(piece, npiece, np * sizeof(u32));
        memcpy(plen, nplen, np * sizeof(u32));
        nf = np;
    }
    /* limite esgotado = area muito danificada: o proximo bloco comeca em modo "zona ruim" */
    *streak = budget <= 0 ? *streak + 1 : 0;
}

static int bk_produce(Pipe *p, Slot *sl)
{
    BkCtx *c = p->ctx;
    Dev *d = c->d;
    u64 off = sl->idx * CHUNK_SIZE, q, pp;
    u32 len = (u32)((d->size - off) < CHUNK_SIZE ? (d->size - off) : CHUNK_SIZE);
    int any_free = 0, any_read = 0, any_zero = 0, f, k;
    sl->len = len;
    sl->stored = sl->crc = 0;
    if (!c->o->raw && g_nareas) {
        for (pp = off; pp < off + len; pp = q) {
            q = seg_next(pp, off + len, &f);
            if (f == 1) any_free = 1;
            else if (f == 2) any_zero = 1;
            else any_read = 1;
        }
    } else {
        any_read = 1;
    }
    if (!any_read && !any_zero) {
        sl->type = CH_FREE;
        c->nfree++;
        return 0;
    }
    if (!any_read) { /* so espaco livre + areas descartadas: nem precisa ler o disco */
        sl->type = CH_ZERO;
        return 0;
    }
    if (dev_pread(d, sl->buf, len, off) != 0) { /* setores defeituosos */
        u64 before = c->unread;
        c->rfails++;
        rescue_read(d, sl->buf, len, off, &c->unread, &c->rfails, &c->bad_streak, c->o->rescue_all);
        c->bad += (c->unread - before) / d->sector;
    } else {
        c->bad_streak = 0;
    }
    for (k = 0; k < g_nzr; k++) { /* inicio de hiberfil/pagefile: gravado como zero */
        u64 zs = g_zr[k].off, ze = g_zr[k].off + g_zr[k].len;
        if (ze > off && zs < off + len) {
            if (zs < off) zs = off;
            if (ze > off + len) ze = off + len;
            memset(sl->buf + (zs - off), 0, (size_t)(ze - zs));
        }
    }
    if (any_free || any_zero) /* zera livres (comprime melhor) e areas descartadas */
        for (pp = off; pp < off + len; pp = q) {
            q = seg_next(pp, off + len, &f);
            if (f) memset(sl->buf + (pp - off), 0, (size_t)(q - pp));
        }
    sl->type = all_zero(sl->buf, len) ? CH_ZERO : T_PENDING;
    return 0;
}

static int bk_work(Pipe *p, Slot *sl, int w)
{
    BkCtx *c = p->ctx;
    if (sl->type != T_PENDING) return 0;
    sl->crc = (u32)crc32(0, sl->buf, sl->len);
    if (!c->o->compat) {
        size_t r = ZSTD_compressCCtx(c->zc[w], sl->cbuf, CBUF_SIZE, sl->buf, sl->len, c->o->level);
        if (!ZSTD_isError(r) && r < sl->len) { sl->type = CH_ZSTD; sl->stored = (u32)r; }
        else { sl->type = CH_RAW; sl->stored = sl->len; }
        return 0;
    }
    /* modo compativel (zlib): testa 64 KB primeiro; se nao comprime, nem tenta o bloco
       inteiro (o zlib e lento em dados ja comprimidos: fotos, videos, zip) */
    if (sl->len >= 65536) {
        uLongf pl = compressBound(65536);
        if (compress2(c->probe[w], &pl, sl->buf, 65536, 1) == Z_OK && pl > 65536u * 97 / 100) {
            sl->type = CH_RAW;
            sl->stored = sl->len;
            return 0;
        }
    }
    {
        uLongf cl = CBUF_SIZE;
        if (compress2(sl->cbuf, &cl, sl->buf, sl->len, c->o->level) == Z_OK && cl < sl->len) {
            sl->type = CH_DEFLATE;
            sl->stored = (u32)cl;
        } else {
            sl->type = CH_RAW;
            sl->stored = sl->len;
        }
    }
    return 0;
}

static int bk_consume(Pipe *p, Slot *sl)
{
    BkCtx *c = p->ctx;
    u8 ch[9];
    ch[0] = sl->type;
    put32(ch + 1, sl->stored);
    put32(ch + 5, sl->crc);
    if (c->idx) memcpy(c->idx + sl->idx * 9, ch, 9);
    if (img_write_nd(c->m, ch, 9) != 0 ||
        (sl->type == CH_DEFLATE || sl->type == CH_ZSTD ? img_write_nd(c->m, sl->cbuf, sl->stored)
         : sl->type == CH_RAW ? img_write_nd(c->m, sl->buf, sl->stored) : 0) != 0) {
        pipe_fail(p, c->m->err);
        return -1;
    }
    if (sl->type != CH_FREE && sl->type != CH_ZERO) c->used += sl->len;
    p->done_bytes += sl->len;
    return 0;
}

typedef struct {
    u32 version, sector, chunk, flags;
    u64 size, nchunks, created;
    char src[129], tool[33];
} Hdr;

static void read_header(Img *m, Hdr *h)
{
    u8 b[HDR_SIZE];
    if (img_read(m, b, HDR_SIZE) != 0) die("arquivo de imagem vazio ou ilegivel");
    if (memcmp(b, "HDCLONE", 8) != 0) die("este arquivo nao e uma imagem do hdclone");
    if (le32(b + 508) != (u32)crc32(0, b, 508)) die("cabecalho da imagem corrompido");
    h->version = le32(b + 8);
    if (h->version < 1 || h->version > 2)
        die("esta imagem foi criada por uma versao mais nova do hdclone; atualize o programa");
    h->sector = le32(b + 12);
    h->chunk = le32(b + 16);
    h->flags = le32(b + 20);
    h->size = le64(b + 24);
    h->nchunks = le64(b + 32);
    h->created = le64(b + 40);
    memcpy(h->src, b + 48, 128); h->src[128] = 0;
    memcpy(h->tool, b + 176, 32); h->tool[32] = 0;
    if (h->chunk != CHUNK_SIZE || h->nchunks != (h->size + h->chunk - 1) / h->chunk)
        die("cabecalho da imagem invalido");
}

static void print_header(Hdr *h)
{
    char s[32], t[64];
    time_t tt = (time_t)h->created;
    struct tm *tm = localtime(&tt);
    fmt_size(h->size, s);
    if (tm) strftime(t, sizeof(t), "%d/%m/%Y %H:%M:%S", tm); else strcpy(t, "?");
    printf("  Disco original : %s (%llu bytes, setor %u)\n", s, (unsigned long long)h->size, h->sector);
    printf("  Origem         : %s\n", h->src);
    printf("  Criada em      : %s  com %s\n", t, h->tool);
    printf("  Modo           : %s%s\n", (h->flags & 1) ? "inteligente (somente dados usados)" : "setor a setor",
           (h->flags & 2) ? ", copia consistente via VSS" : "");
    printf("  Compressao     : %s\n", h->version >= 2 ? "zstd" : "zlib (formato compativel com versoes antigas)");
}

/* tamanho de cada parte da imagem (-1 = nao existe) */
static i64 part_size(const char *base, int part)
{
    char nm[620];
    part_name(base, part, nm);
#ifdef _WIN32
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(nm, GetFileExInfoStandard, &fa)) return -1;
        return (i64)(((u64)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
    }
#else
    {
        struct stat st;
        if (stat(nm, &st) != 0) return -1;
        return (i64)st.st_size;
    }
#endif
}


/* ---- espaco no destino (antes de comecar) ---- */

/* bytes que o backup vai realmente ler/comprimir (sem espaco livre) */
static u64 bytes_to_copy(u64 size, int raw)
{
    u64 pos, q, n = 0;
    int f;
    if (raw || !g_nareas) return size;
    for (pos = 0; pos < size; pos = q) {
        q = seg_next(pos, size, &f);
        if (f == 0) n += q - pos;
    }
    return n;
}

/* espaco livre e tipo do sistema de arquivos do destino. 0 = ok */
static int dest_info(const char *path, u64 *avail, int *is_fat)
{
#ifdef _WIN32
    char full[MAX_PATH], root[MAX_PATH], fs[32] = "";
    ULARGE_INTEGER av, tot, fr;
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL) || !GetVolumePathNameA(full, root, MAX_PATH)) return -1;
    if (!GetDiskFreeSpaceExA(root, &av, &tot, &fr)) return -1;
    *avail = av.QuadPart;
    GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, sizeof(fs));
    *is_fat = !strncmp(fs, "FAT", 3);
    return 0;
#else
    char dir[620];
    struct statvfs sv;
    struct statfs sf;
    char *sl;
    snprintf(dir, sizeof(dir), "%s", path);
    sl = strrchr(dir, '/');
    if (sl) *sl = 0; else strcpy(dir, ".");
    if (!dir[0]) strcpy(dir, "/");
    if (statvfs(dir, &sv) != 0) return -1;
    *avail = (u64)sv.f_bavail * sv.f_frsize;
    *is_fat = statfs(dir, &sf) == 0 && sf.f_type == 0x4d44; /* MSDOS_SUPER_MAGIC */
    return 0;
#endif
}

/* confere antes de copiar: espaco claramente insuficiente = para ja; FAT32 = divide */
static void check_dest_space(const char *dst, u64 data, Opts *o)
{
    u64 avail = 0, have = 0;
    int is_fat = 0, k;
    i64 ps;
    char s1[32], s2[32], msg[400];
    if (dest_info(dst, &avail, &is_fat) != 0) return;
    for (k = 1; k < 1000 && (ps = part_size(dst, k)) >= 0; k++) have += (u64)ps; /* imagem antiga sera substituida */
    if (!o->resume) avail += have;
    fmt_size(data, s1);
    fmt_size(avail, s2);
    printf("\nDados a copiar: %s   Espaco livre no destino: %s\n", s1, s2);
    if (is_fat && (!o->split || o->split > 4000ull * 1024 * 1024)) {
        o->split = 4000ull * 1024 * 1024;
        printf("  O destino e FAT32 (arquivos de no maximo 4 GB): a imagem sera dividida em partes\n"
               "  de 4000 MB automaticamente.\n");
    }
    if (avail < data / 10 * 3) { /* nem com compressao muito boa caberia */
        snprintf(msg, sizeof(msg), "espaco insuficiente no destino: %s livres para %s de dados (a imagem costuma "
                 "ficar entre 40%% e 100%% dos dados). Libere espaco ou escolha outro destino.", s2, s1);
        die(msg);
    }
    if (avail < data / 4 * 3)
        printf("  AVISO: pode nao caber. A imagem costuma ficar entre 40%% e 100%% dos dados,\n"
               "  conforme o tipo de arquivo (fotos e videos quase nao comprimem).\n");
}

/* ---- retomar backup interrompido ---- */

typedef struct {
    u64 k;          /* blocos completos e conferidos */
    int part;       /* onde a gravacao continua */
    u64 off;
    u64 used, nfree;
    int complete;
} ResumeInfo;

static void resume_scan(const char *dst, Dev *d, Opts *o, u8 *ixraw, ResumeInfo *ri)
{
    Img m;
    Hdr h;
    u64 i;
    struct { int hp, pp; u64 ho, po; u8 type; u32 stored, crc, len; } last[16];
    int nl = 0, j;
    u8 *buf = NULL, *cb = NULL;
    memset(ri, 0, sizeof(*ri));
    if (img_open(&m, dst, 0, 0) != 0) die("nao ha backup para continuar (arquivo de imagem nao encontrado)");
    read_header(&m, &h);
    if (h.size != d->size || h.sector != d->sector || (int)h.version != (o->compat ? 1 : 2) ||
        (int)(h.flags & 1) != !o->raw)
        die("o backup existente e de outro disco ou com outras opcoes; nao da para continuar (comece um novo)");
    ri->part = 1;
    ri->off = HDR_SIZE;
    for (i = 0; i < h.nchunks; i++) {
        u8 ch[9];
        int hp = m.part;
        u64 ho = m.pos, len = (h.size - i * CHUNK_SIZE) < CHUNK_SIZE ? (h.size - i * CHUNK_SIZE) : CHUNK_SIZE;
        if (img_read(&m, ch, 9) != 0 || ch[0] > CH_ZSTD || le32(ch + 1) > CBUF_SIZE) break;
        if (ch[0] == CH_DEFLATE || ch[0] == CH_RAW || ch[0] == CH_ZSTD) {
            int pp = m.part;
            u64 po = m.pos;
            if (img_skip(&m, le32(ch + 1)) != 0) break;
            if (m.part > pp || m.pos > po) { /* guarda os ultimos 16 blocos com dados para conferir */
                if (nl == 16) { memmove(last, last + 1, sizeof(last[0]) * 15); nl = 15; }
                last[nl].hp = hp; last[nl].ho = ho; last[nl].pp = pp; last[nl].po = po;
                last[nl].type = ch[0]; last[nl].stored = le32(ch + 1); last[nl].crc = le32(ch + 5);
                last[nl].len = (u32)len;
                nl++;
            }
            ri->used += len;
        } else if (ch[0] == CH_FREE) {
            ri->nfree++;
        }
        memcpy(ixraw + i * 9, ch, 9);
        ri->k = i + 1;
        ri->part = m.part;
        ri->off = m.pos;
        if (m.pos == m.psize) { /* fim exato de uma parte: continua na proxima */
            i64 ps = part_size(dst, m.part + 1);
            if (ps < 0) { ri->part = m.part; ri->off = m.pos; }
        }
    }
    if (ri->k == h.nchunks) {
        u8 f[6];
        if (img_read(&m, f, 6) == 0 && !memcmp(f, "HDCEND", 6)) ri->complete = 1;
    }
    /* o final pode ter sido gravado pela metade: confere o CRC dos ultimos blocos */
    buf = xalloc(CHUNK_SIZE);
    cb = xalloc(CBUF_SIZE);
    for (j = 0; j < nl && !ri->complete; j++) {
        uLongf ul = CHUNK_SIZE;
        int ok = img_seek(&m, last[j].pp, last[j].po) == 0 && img_read(&m, cb, last[j].stored) == 0;
        if (ok) {
            if (last[j].type == CH_ZSTD) ok = ZSTD_decompress(buf, CHUNK_SIZE, cb, last[j].stored) == last[j].len;
            else if (last[j].type == CH_DEFLATE) ok = uncompress(buf, &ul, cb, last[j].stored) == Z_OK && ul == last[j].len;
            else memcpy(buf, cb, last[j].len);
            ok = ok && (u32)crc32(0, buf, last[j].len) == last[j].crc;
        }
        if (!ok) { /* corta a partir deste bloco */
            u64 k2 = ri->k;
            int q;
            for (q = j; q < nl; q++) k2--;
            ri->k = k2;
            ri->part = last[j].hp;
            ri->off = last[j].ho;
            printf("  Os ultimos blocos gravados estavam incompletos e serao refeitos.\n");
            break;
        }
    }
    xfree(buf);
    xfree(cb);
    img_close_part(&m);
}

/* abre a imagem para continuar gravando em (parte, posicao); apaga o que vem depois */
static void img_open_append(Img *m, const char *path, u64 limit, int part, u64 off)
{
    char nm[620];
    int k;
    memset(m, 0, sizeof(*m));
    strncpy(m->base, path, sizeof(m->base) - 1);
    m->part = part;
    m->limit = limit;
    m->direct = 0; /* posicao nao alinhada: grava pelo cache nesta execucao */
    m->wbuf = xalloc(WBUF + WALIGN);
    for (k = part + 1; k < 1000; k++) { /* partes depois do ponto de corte */
        part_name(path, k, nm);
        if (part_size(path, k) < 0) break;
#ifdef _WIN32
        DeleteFileA(nm);
#else
        unlink(nm);
#endif
    }
    part_name(path, part, nm);
#ifdef _WIN32
    {
        LARGE_INTEGER li;
        m->h = CreateFileA(nm, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (m->h == INVALID_HANDLE_VALUE) die("nao foi possivel abrir a imagem para continuar");
        res_add(RES_HANDLE, m->h);
        li.QuadPart = (LONGLONG)off;
        if (!SetFilePointerEx(m->h, li, NULL, FILE_BEGIN) || !SetEndOfFile(m->h)) die("nao foi possivel preparar a imagem");
    }
#else
    m->fd = open(nm, O_WRONLY | O_CREAT, 0644);
    if (m->fd < 0) die("nao foi possivel abrir a imagem para continuar");
    res_add(RES_HANDLE, (void *)(intptr_t)(m->fd + 1));
    if (ftruncate(m->fd, (off_t)off) != 0 || lseek(m->fd, (off_t)off, SEEK_SET) < 0) die("nao foi possivel preparar a imagem");
#endif
    m->cur = off;
}

/* ---- indice no fim da imagem ----
   Depois do rodape: os cabecalhos de 9 bytes de todos os blocos (comprimidos com
   zstd) + um trecho final de 64 bytes. Versoes antigas param no rodape e ignoram.
   Com ele, abrir a imagem para recuperar arquivos (ou restaurar em disco menor) le
   alguns KB em vez de percorrer a imagem inteira. */
#define IDX_TRAILER 64

static void img_write_index(Img *m, const u8 *raw, u64 nchunks)
{
    u64 rawlen = nchunks * 9, off;
    size_t cap = ZSTD_compressBound((size_t)rawlen), cl;
    u8 *cz = malloc(cap), t[IDX_TRAILER];
    if (!cz) return; /* sem indice: a imagem continua valida */
    cl = ZSTD_compress(cz, cap, raw, (size_t)rawlen, 3);
    if (ZSTD_isError(cl)) { free(cz); return; }
    off = m->total;
    img_write(m, cz, cl);
    free(cz);
    memset(t, 0, sizeof(t));
    memcpy(t, "HDCIDX\0\1", 8);
    put64(t + 8, nchunks);
    put64(t + 16, rawlen);
    put64(t + 24, cl);
    put64(t + 32, off);
    put32(t + 40, (u32)crc32(0, raw, (uInt)rawlen));
    put32(t + 60, (u32)crc32(0, t, 60));
    img_write(m, t, IDX_TRAILER);
}

static void cmd_backup(const char *src, const char *dst, Opts *o)
{
    Dev d;
    Img m;
    u8 hdr[HDR_SIZE], ftr[FTR_SIZE], *ixraw = NULL;
    u64 nchunks, used = 0, bad = 0, nfree = 0;
    ResumeInfo ri;
    char s1[32], s2[32];

    {
        char lp[700];
        snprintf(lp, sizeof(lp), "%s.log", dst);
        log_open(lp, o->resume ? "BACKUP (retomada)" : "BACKUP", o->resume);
    }
    if (dev_open(&d, src, 0) != 0) die("nao foi possivel abrir o disco de origem (execute como Administrador)");
    if (!d.size) die("nao foi possivel obter o tamanho do disco de origem");
    fmt_size(d.size, s1);
    printf("\nOrigem : %s  (%s, setor %u bytes)\n", d.name, s1, d.sector);
    printf("Destino: %s\n", dst);

    if (d.disknum >= 0 && path_disk(dst) == d.disknum)
        die("a imagem nao pode ser gravada no mesmo disco que esta sendo copiado");

    g_vss_used = 0;
#ifdef _WIN32
    if (d.disknum >= 0 && !o->no_vss) {
#else
    if (getenv("HDCLONE_TEST_SNAP") && !o->no_vss) {
#endif
        printf("\nVSS (copia consistente com o Windows ligado):\n");
        if (vss_begin(d.disknum) == 0) {
            d.use_snap = 1;
            g_vss_used = 1;
        } else {
            printf("  AVISO: VSS indisponivel - %s\n", g_vss_note);
        }
    }

    if (!o->raw) {
        printf("\nAnalisando particoes...\n");
        g_keep_sys = o->keep_sys;
        g_skip_restore = o->skip_restore;
        reset_areas();
        scan_partitions(&d);
    } else {
        printf("\nModo setor a setor (--raw): todo o disco sera copiado\n");
    }

    if (d.disknum >= 0) {
        printf("\nBloqueando volumes sem snapshot...\n");
        if (lock_disk_volumes(d.disknum, 0) > 0)
            printf("  AVISO: ha volumes em uso. Arquivos abertos podem ficar inconsistentes na imagem.\n"
                   "  Para um clone 100%% consistente do disco do Windows, rode o hdclone a partir\n"
                   "  de um boot externo (WinPE / Hiren's BootCD PE) ou com o HD em outro PC.\n");
    }

    nchunks = (d.size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    ixraw = xalloc((size_t)nchunks * 9);
    memset(&ri, 0, sizeof(ri));
    if (o->resume && part_size(dst, 1) < HDR_SIZE) { /* nada foi gravado antes da interrupcao */
        printf("\nO backup anterior nao chegou a gravar dados: comecando do inicio.\n");
        o->resume = 0;
    }
    if (o->resume) {
        u64 g = 0;
        int k;
#ifdef _WIN32
        if (d.disknum >= 0 && d.disknum == system_disk())
            die("nao e seguro continuar o backup do disco do Windows em uso: os dados mudaram desde a "
                "interrupcao e a imagem misturaria dois momentos. Comece um backup novo.");
#endif
        printf("\nProcurando o ponto onde o backup parou...\n");
        resume_scan(dst, &d, o, ixraw, &ri);
        if (ri.complete) die("este backup ja esta completo; nao ha o que continuar");
        printf("  %llu de %llu MB ja estao na imagem; continuando a partir dai.\n", (unsigned long long)ri.k,
               (unsigned long long)nchunks);
        if (d.disknum >= 0 || g_vss_used)
            printf("  AVISO: se o disco de origem foi usado depois da interrupcao, a imagem pode ficar\n"
                   "  inconsistente. Na duvida, comece um backup novo.\n");
        img_open_append(&m, dst, o->split, ri.part, ri.off);
        for (k = 1; k < ri.part; k++) g += (u64)part_size(dst, k);
        m.total = g + ri.off;
    } else {
        check_dest_space(dst, bytes_to_copy(d.size, o->raw), o);
        if (img_open(&m, dst, 1, o->split) != 0) die("nao foi possivel criar o arquivo de imagem");
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "HDCLONE", 8);
    put32(hdr + 8, o->compat ? 1 : 2); /* 2 = pode conter blocos zstd */
    put32(hdr + 12, d.sector);
    put32(hdr + 16, CHUNK_SIZE);
    put32(hdr + 20, (o->raw ? 0 : 1) | (g_vss_used ? 2 : 0));
    put64(hdr + 24, d.size);
    put64(hdr + 32, nchunks);
    put64(hdr + 40, (u64)time(NULL));
    memcpy(hdr + 48, d.name, strlen(d.name) < 127 ? strlen(d.name) : 127);
    strncpy((char *)hdr + 176, VERSION_STR, 31);
    put32(hdr + 508, (u32)crc32(0, hdr, 508));
    if (!o->resume) img_write(&m, hdr, HDR_SIZE);

    {
        Pipe pp;
        BkCtx bc;
        int k, rc;
        memset(&bc, 0, sizeof(bc));
        bc.d = &d;
        bc.m = &m;
        bc.o = o;
        bc.idx = ixraw;
        bc.used = ri.used;
        bc.nfree = ri.nfree;
        pipe_init(&pp, nchunks - ri.k, d.size);
        pp.first = ri.k;
        pp.done_bytes = ri.k * CHUNK_SIZE;
        pp.produce = bk_produce;
        pp.work = bk_work;
        pp.consume = bk_consume;
        pp.ctx = &bc;
        for (k = 0; k < pp.N; k++) {
            if (!o->compat) bc.zc[k] = ZSTD_createCCtx();
            else bc.probe[k] = xalloc(compressBound(65536));
        }
        g_last_direct = m.direct;
        printf("\nCopiando (%s nivel %d, %d thread%s de compressao%s)...\n", o->compat ? "zlib" : "zstd", o->level,
               pp.N, pp.N > 1 ? "s" : "", m.direct ? ", gravacao direta" : "");
        rc = pipe_run(&pp);
        for (k = 0; k < pp.N; k++) {
            ZSTD_freeCCtx(bc.zc[k]);
            xfree(bc.probe[k]);
        }
        pipe_free(&pp);
        if (rc != 0) die(pp.err);
        used = bc.used;
        bad = bc.bad;
        nfree = bc.nfree;
        g_last_rfails = bc.rfails;
    }

    memset(ftr, 0, sizeof(ftr));
    memcpy(ftr, "HDCEND", 6);
    put64(ftr + 8, nchunks);
    put64(ftr + 16, used);
    put64(ftr + 24, m.total + FTR_SIZE);
    put64(ftr + 32, bad);
    put32(ftr + 44, (u32)crc32(0, ftr, 44));
    img_write(&m, ftr, FTR_SIZE);
    img_write_index(&m, ixraw, nchunks);
    xfree(ixraw);
    img_finish(&m);
    unlock_all();
    vss_end(1);
    dev_close(&d);

    fmt_size(d.size, s1);
    fmt_size(m.total, s2);
    printf("\n\nBackup concluido!\n");
    printf("  Disco: %s   Imagem: %s (%.1f%% do disco)   Partes: %d\n", s1, s2,
           100.0 * (double)m.total / (double)d.size, m.part);
    if (nfree) printf("  Blocos de espaco livre ignorados: %llu MB\n", (unsigned long long)nfree);
    if (bad) printf("  ATENCAO: %llu setores ilegiveis (ou pulados em area ruim) foram gravados como zero!\n"
                    "  %llu tentativas de leitura falharam.\n", (unsigned long long)bad, (unsigned long long)g_last_rfails);
#ifndef _WIN32
    if (getenv("HDCLONE_TEST_BAD")) printf("  [teste] leituras com falha: %llu\n", (unsigned long long)g_tb_fails);
#endif
    if (g_vss_used) printf("  Copia consistente feita via VSS (snapshots temporarios ja removidos).\n");
    reset_areas();
    if (o->verify) {
        printf("\nVerificando a imagem gravada (lendo de volta do disco)...\n");
        cmd_verify(dst);
    }
    log_close(o->verify ? "OK (imagem gravada e verificada)" : "OK");
}
