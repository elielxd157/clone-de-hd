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

typedef struct {
    Img m;
    Hdr h;
    ChunkIdx *ix;
    u8 *cache[IMG_CACHE], *cbuf;
    i64 ci[IMG_CACHE];
    u64 used[IMG_CACHE], tick;
} ImgSrc;

static ImgSrc g_is;

/* le o indice do fim da imagem; 0 = ok (s->ix preenchido) */
static int imgsrc_try_index(ImgSrc *s, const char *path)
{
    u64 sizes[1000], total = 0, g, rawlen, cl, off, i;
    int np = 0, k, part;
    u8 t[IDX_TRAILER], *cz = NULL, *raw = NULL;
    i64 sz;
    while (np < 1000 && (sz = part_size(path, np + 1)) >= 0) { sizes[np++] = (u64)sz; total += (u64)sz; }
    if (!np || total < HDR_SIZE + FTR_SIZE + IDX_TRAILER) return -1;
#define G2P(gv, pp, oo)                                                   \
    do {                                                                  \
        u64 _g = (gv);                                                    \
        for (pp = 0; pp < np - 1 && _g >= sizes[pp]; pp++) _g -= sizes[pp]; \
        oo = _g;                                                          \
    } while (0)
    G2P(total - IDX_TRAILER, k, off);
    if (img_seek(&s->m, k + 1, off) != 0 || img_read(&s->m, t, IDX_TRAILER) != 0) return -1;
    if (memcmp(t, "HDCIDX\0\1", 8) != 0 || le32(t + 60) != (u32)crc32(0, t, 60)) return -1;
    rawlen = le64(t + 16);
    cl = le64(t + 24);
    if (le64(t + 8) != s->h.nchunks || rawlen != s->h.nchunks * 9 || le64(t + 32) + cl + IDX_TRAILER != total ||
        cl > 64ull * 1024 * 1024)
        return -1;
    cz = malloc((size_t)cl + 1);
    raw = malloc((size_t)rawlen + 1);
    if (!cz || !raw) goto fail;
    G2P(le64(t + 32), k, off);
    if (img_seek(&s->m, k + 1, off) != 0 || img_read(&s->m, cz, cl) != 0) goto fail;
    if (ZSTD_decompress(raw, (size_t)rawlen, cz, (size_t)cl) != rawlen || le32(t + 40) != (u32)crc32(0, raw, (uInt)rawlen))
        goto fail;
    g = HDR_SIZE;
    for (i = 0; i < s->h.nchunks; i++) {
        ChunkIdx *c = &s->ix[i];
        c->type = raw[i * 9];
        c->stored = le32(raw + i * 9 + 1);
        c->crc = le32(raw + i * 9 + 5);
        if (c->type > CH_ZSTD || c->stored > CBUF_SIZE) goto fail;
        g += 9;
        if (c->type == CH_DEFLATE || c->type == CH_RAW || c->type == CH_ZSTD) {
            u64 o2;
            G2P(g, part, o2);
            c->part = part + 1;
            c->pos = o2;
            g += c->stored;
        }
    }
    { /* confere: o rodape tem que estar logo depois do ultimo bloco */
        u8 f[6];
        G2P(g, k, off);
        if (img_seek(&s->m, k + 1, off) != 0 || img_read(&s->m, f, 6) != 0 || memcmp(f, "HDCEND", 6) != 0) goto fail;
    }
#undef G2P
    free(cz);
    free(raw);
    return 0;
fail:
    free(cz);
    free(raw);
    return -1;
}

/* percorre a imagem lendo so os cabecalhos dos blocos e monta um indice */
static void imgsrc_open(const char *path)
{
    ImgSrc *s = &g_is;
    u64 i = 0;
    memset(s, 0, sizeof(*s));
    if (img_open(&s->m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&s->m, &s->h);
    s->ix = xalloc((size_t)s->h.nchunks * sizeof(ChunkIdx));
    g_t0 = g_tlast = now_ms();
    if (imgsrc_try_index(s, path) == 0) {
        printf("  (indice da imagem lido)\n");
        i = s->h.nchunks;
    } else if (img_seek(&s->m, 1, HDR_SIZE) != 0) {
        die("nao foi possivel ler a imagem");
    }
    for (; i < s->h.nchunks; i++) {
        u8 ch[9];
        ChunkIdx *c = &s->ix[i];
        if (g_cancel) die("Operacao cancelada pelo usuario.");
        if (img_read(&s->m, ch, 9) != 0) die("imagem incompleta (falta alguma parte?)");
        c->type = ch[0];
        c->stored = le32(ch + 1);
        c->crc = le32(ch + 5);
        if (c->type > CH_ZSTD || c->stored > CBUF_SIZE) die("imagem corrompida");
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
}

static void imgsrc_close(void)
{
    ImgSrc *s = &g_is;
    int i;
    img_close_part(&s->m);
    xfree(s->ix);
    for (i = 0; i < IMG_CACHE; i++) xfree(s->cache[i]);
    xfree(s->cbuf);
    memset(s, 0, sizeof(*s));
}

/* devolve o bloco i descomprimido (cache LRU) ou NULL */
static u8 *imgsrc_chunk(ImgSrc *s, u64 i)
{
    ChunkIdx *c = &s->ix[i];
    u64 off = i * CHUNK_SIZE;
    u32 len = (u32)((s->h.size - off) < CHUNK_SIZE ? (s->h.size - off) : CHUNK_SIZE);
    int k, v = 0;
    u8 *dst;
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
