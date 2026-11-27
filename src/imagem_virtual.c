/* hdclone - cabecalho, imagem como disco virtual */

/* ------------------------------------------------------------------ */
/* leitura do cabecalho                                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* VERIFY / RESTORE                                                    */
/* ------------------------------------------------------------------ */

/* reescreve a GPT para o tamanho do disco de destino (maior ou menor que o
   original): cabecalho primario, entradas e copia de seguranca no fim do disco */
static void fix_gpt(Dev *d)
{
    u32 ss = d->sector, hsize, n, esz, esec, i;
    u64 last = d->size / ss - 1, alt, elba;
    u8 *h = xalloc(ss), *b = xalloc(ss), *mbr = xalloc(ss), *e;

    if (dev_pread(d, mbr, ss, 0) != 0 || dev_pread(d, h, ss, ss) != 0 || memcmp(h, "EFI PART", 8) != 0)
        goto out;
    hsize = le32(h + 12);
    alt = le64(h + 32);
    elba = le64(h + 72);
    n = le32(h + 80);
    esz = le32(h + 84);
    if (hsize < 92 || hsize > ss || alt == last || n > 4096 || esz < 128) goto out;
    esec = (u32)(((u64)n * esz + ss - 1) / ss);
    e = xalloc((size_t)esec * ss);
    if (dev_pread(d, e, esec * ss, elba * ss) == 0) {
        put32(h + 88, (u32)crc32(0, e, n * esz)); /* entradas podem ter sido alteradas */
        if (dev_pwrite(d, e, esec * ss, elba * ss) != 0) printf("  AVISO: falha ao gravar entradas GPT\n");
        put64(h + 32, last);
        put64(h + 48, last - esec - 1);
        put32(h + 16, 0);
        put32(h + 16, (u32)crc32(0, h, hsize));
        memcpy(b, h, ss);
        put64(b + 24, last);
        put64(b + 32, 1);
        put64(b + 72, last - esec);
        put32(b + 16, 0);
        put32(b + 16, (u32)crc32(0, b, hsize));
        if (dev_pwrite(d, e, esec * ss, (last - esec) * ss) == 0 &&
            dev_pwrite(d, b, ss, last * ss) == 0 &&
            dev_pwrite(d, h, ss, ss) == 0) {
            for (i = 0; i < 4; i++) {
                u8 *p = mbr + 446 + 16 * i;
                if (p[4] == 0xEE) put32(p + 12, last > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)last);
            }
            dev_pwrite(d, mbr, ss, 0);
            printf("  GPT ajustada para o novo tamanho do disco.\n");
        } else {
            printf("  AVISO: nao foi possivel ajustar a GPT de backup.\n");
        }
    }
    xfree(e);
out:
    xfree(h);
    xfree(b);
    xfree(mbr);
}

/* ------------------------------------------------------------------ */
/* imagem como "disco virtual" (acesso aleatorio)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    u64 pos;      /* posicao dos dados na parte */
    u32 stored, crc;
    int part;
    u8 type;
} ChunkIdx;

#define IMG_CACHE 8 /* blocos descomprimidos em cache (MFT + dados alternam muito) */

typedef struct ImgSrc {
    Img m;
    Hdr h;
    ChunkIdx *ix;
    u8 *cache[IMG_CACHE], *cbuf;
    i64 ci[IMG_CACHE];
    u64 used[IMG_CACHE], tick;
    struct ImgSrc *parent; /* incremental: imagem base */
} ImgSrc;

static ImgSrc g_is;
static int g_idx_used; /* ultima imagem aberta pelo indice */

/* le o indice do fim da imagem; 0 = ok (s->ix preenchido) */
static int imgsrc_try_index(ImgSrc *s, const char *path)
{
    u64 sizes[1000], g, i;
    int np = 0, part;
    u8 *raw;
    i64 sz;
    if (idx_read(&s->m, path, s->h.nchunks, &raw, NULL) != 0) return -1;
    while (np < 1000 && (sz = part_size(path, np + 1)) >= 0) sizes[np++] = (u64)sz;
    g = HDR_SIZE;
    for (i = 0; i < s->h.nchunks; i++) {
        ChunkIdx *c = &s->ix[i];
        c->type = raw[i * 9];
        c->stored = le32(raw + i * 9 + 1);
        c->crc = le32(raw + i * 9 + 5);
        if (c->type > CH_SAME || c->stored > CBUF_SIZE) goto fail;
        g += 9;
        if (c->type == CH_DEFLATE || c->type == CH_RAW || c->type == CH_ZSTD) {
            g2p(sizes, np, g, &part, &c->pos);
            c->part = part;
            g += c->stored;
        }
    }
    { /* confere: o rodape tem que estar logo depois do ultimo bloco */
        u8 f[6];
        u64 off;
        g2p(sizes, np, g, &part, &off);
        if (img_seek(&s->m, part, off) != 0 || img_read(&s->m, f, 6) != 0 || memcmp(f, "HDCEND", 6) != 0) goto fail;
    }
    free(raw);
    return 0;
fail:
    free(raw);
    return -1;
}

static void imgsrc_close_s(ImgSrc *s)
{
    int i;
    if (s->parent) { imgsrc_close_s(s->parent); free(s->parent); }
    img_close_part(&s->m);
    xfree(s->ix);
    for (i = 0; i < IMG_CACHE; i++) xfree(s->cache[i]);
    xfree(s->cbuf);
    memset(s, 0, sizeof(*s));
}

/* abre a imagem (e as imagens base, se for incremental) para acesso aleatorio */
static void imgsrc_init(ImgSrc *s, const char *path, int depth)
{
    u64 i = 0;
    memset(s, 0, sizeof(*s));
    if (img_open(&s->m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&s->m, &s->h);
    if (depth == 0) check_chain(path, &s->h);
    s->ix = xalloc((size_t)s->h.nchunks * sizeof(ChunkIdx));
    g_t0 = g_tlast = now_ms();
    if (imgsrc_try_index(s, path) == 0) {
        if (!depth) printf("  (indice da imagem lido)\n");
        if (!depth) g_idx_used = 1;
        i = s->h.nchunks;
    } else if (img_seek(&s->m, 1, HDR_SIZE) != 0) {
        die("nao foi possivel ler a imagem");
    }
    if (i < s->h.nchunks && depth) printf("  lendo a imagem base %s...\n", base_name(path));
    for (; i < s->h.nchunks; i++) {
        u8 ch[9];
        ChunkIdx *c = &s->ix[i];
        if (g_cancel) die("Operacao cancelada pelo usuario.");
        if (img_read(&s->m, ch, 9) != 0) die("imagem incompleta (falta alguma parte?)");
        c->type = ch[0];
        c->stored = le32(ch + 1);
        c->crc = le32(ch + 5);
        if (c->type > CH_SAME || c->stored > CBUF_SIZE) die("imagem corrompida");
        if (c->type == CH_DEFLATE || c->type == CH_RAW || c->type == CH_ZSTD) {
            c->part = s->m.part;
            c->pos = s->m.pos;
            if (img_skip(&s->m, c->stored) != 0) die("imagem incompleta (falta alguma parte?)");
        }
        progress((i + 1) * CHUNK_SIZE < s->h.size ? (i + 1) * CHUNK_SIZE : s->h.size, s->h.size,
                 i + 1 == s->h.nchunks);
    }
    for (i = 0; i < IMG_CACHE; i++) {
        s->cache[i] = xalloc(CHUNK_SIZE);
        s->ci[i] = -1;
    }
    s->cbuf = xalloc(CBUF_SIZE);
    if (s->h.flags & HF_INCR) { /* a cadeia ja foi conferida em check_chain */
        char pp[700];
        sibling_path(path, s->h.pname, pp, sizeof(pp));
        s->parent = calloc(1, sizeof(ImgSrc));
        if (!s->parent) die("memoria insuficiente");
        imgsrc_init(s->parent, pp, depth + 1);
    }
}

static void imgsrc_open(const char *path) { imgsrc_init(&g_is, path, 0); }
static void imgsrc_close(void) { imgsrc_close_s(&g_is); }

/* devolve o bloco i descomprimido (cache LRU) ou NULL */
static u8 *imgsrc_chunk(ImgSrc *s, u64 i)
{
    ChunkIdx *c = &s->ix[i];
    u64 off = i * CHUNK_SIZE;
    u32 len = (u32)((s->h.size - off) < CHUNK_SIZE ? (s->h.size - off) : CHUNK_SIZE);
    int k, v = 0;
    u8 *dst;
    if (c->type == CH_SAME) { /* igual ao bloco da imagem base */
        u8 *p = s->parent ? imgsrc_chunk(s->parent, i) : NULL;
        return p && (u32)crc32(0, p, len) == c->crc ? p : NULL;
    }
    for (k = 0; k < IMG_CACHE; k++)
        if (s->ci[k] == (i64)i) { s->used[k] = ++s->tick; return s->cache[k]; }
    for (k = 1; k < IMG_CACHE; k++)
        if (s->used[k] < s->used[v]) v = k;
    dst = s->cache[v];
    s->ci[v] = -1;
    if (c->type == CH_ZERO || c->type == CH_FREE) {
        memset(dst, 0, len);
    } else {
        if (img_seek(&s->m, c->part, c->pos) != 0) return NULL;
        if (c->type == CH_RAW) {
            if (img_read(&s->m, dst, len) != 0) return NULL;
        } else if (c->type == CH_ZSTD) {
            if (img_read(&s->m, s->cbuf, c->stored) != 0) return NULL;
            if (ZSTD_decompress(dst, CHUNK_SIZE, s->cbuf, c->stored) != len) return NULL;
        } else {
            uLongf ul = CHUNK_SIZE;
            if (img_read(&s->m, s->cbuf, c->stored) != 0) return NULL;
            if (uncompress(dst, &ul, s->cbuf, c->stored) != Z_OK || ul != len) return NULL;
        }
        if ((u32)crc32(0, dst, len) != c->crc) return NULL;
    }
    s->ci[v] = (i64)i;
    s->used[v] = ++s->tick;
    return dst;
}

static int imgsrc_pread(void *src, void *buf, u32 len, u64 off)
{
    ImgSrc *s = src;
    u8 *p = buf;
    while (len) {
        u64 i = off / CHUNK_SIZE;
        u32 o = (u32)(off % CHUNK_SIZE), n = CHUNK_SIZE - o;
        if (n > len) n = len;
        u8 *c;
        if (off + n > s->h.size || !(c = imgsrc_chunk(s, i))) return -1;
        memcpy(p, c + o, n);
        p += n; off += n; len -= n;
    }
    return 0;
}
