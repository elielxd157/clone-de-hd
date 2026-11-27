/* hdclone - geometria e mapa de clusters usados (NTFS/FAT32) */

/* ------------------------------------------------------------------ */
/* mapa de clusters usados (NTFS / FAT32)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u64 start; /* offset em bytes no disco do cluster 0 da area */
    u64 unit;  /* bytes por cluster */
    u64 count; /* numero de clusters */
    u8 *bm;    /* bit 1 = cluster usado */
    u8 *zb;    /* bit 1 = gravar como zeros (ex.: pontos de restauracao), NULL = nenhum */
} Area;

#define MAX_AREAS 128
static Area g_areas[MAX_AREAS];
static int g_nareas;

static int g_nzr;
static void reset_areas(void)
{
    int i;
    g_nzr = 0;
    for (i = 0; i < g_nareas; i++) { free(g_areas[i].bm); free(g_areas[i].zb); }
    g_nareas = 0;
}

/* libera tudo o que ficou aberto (usado quando uma operacao e abortada) */
static void unlock_all(void);
static void cleanup_all(void)
{
    int i;
    unlock_all();
    vss_end(0);
    for (i = 0; i < MAX_RES; i++) {
        void *p = g_res[i].p;
        if (!p) continue;
        g_res[i].p = NULL;
        if (g_res[i].kind == RES_MEM) {
#ifdef _WIN32
            VirtualFree(p, 0, MEM_RELEASE);
#else
            free(p);
#endif
        } else {
#ifdef _WIN32
            CloseHandle((HANDLE)p);
#else
            close((int)(intptr_t)p - 1);
#endif
        }
    }
    reset_areas();
}

static int bm_get(const u8 *bm, u64 i) { return (bm[i >> 3] >> (i & 7)) & 1; }

static void add_area(u64 start, u64 unit, u64 count, u8 *bm)
{
    if (g_nareas >= MAX_AREAS) { free(bm); return; }
    g_areas[g_nareas].start = start;
    g_areas[g_nareas].unit = unit;
    g_areas[g_nareas].count = count;
    g_areas[g_nareas].bm = bm;
    g_areas[g_nareas].zb = NULL;
    g_nareas++;
}

static u64 count_used(const u8 *bm, u64 n)
{
    u64 i, c = 0;
    for (i = 0; i < n; i++) c += bm_get(bm, i);
    return c;
}

/* ---- NTFS ---- */

typedef struct {
    u32 bps, cs, recsz;
    u64 tot_sec, ncl, mft_lcn, mirr_lcn;
} NtfsGeo;

typedef struct {
    u64 len;  /* clusters */
    i64 lcn;  /* -1 = esparso */
} Run;

static int ntfs_geo(const u8 *b, u64 plen, NtfsGeo *g)
{
    u32 spc;
    u8 spc_raw = b[13];
    signed char cpr = (signed char)b[0x40];
    memset(g, 0, sizeof(*g));
    g->bps = le16(b + 11);
    spc = spc_raw <= 0x80 ? spc_raw : (1u << (256 - spc_raw));
    if (g->bps < 512 || g->bps > 4096 || (g->bps & (g->bps - 1)) || !spc) return -1;
    g->cs = g->bps * spc;
    g->tot_sec = le64(b + 0x28);
    g->mft_lcn = le64(b + 0x30);
    g->mirr_lcn = le64(b + 0x38);
    g->recsz = cpr > 0 ? (u32)(cpr * g->cs) : (1u << (-cpr));
    if (g->recsz < 256 || g->recsz > 65536) return -1;
    g->ncl = g->tot_sec * g->bps / g->cs;
    if (g->ncl == 0 || g->ncl * g->cs > plen || g->mft_lcn >= g->ncl) return -1;
    return 0;
}

/* desfaz a protecao "update sequence" (registros FILE e blocos INDX) */
static int ntfs_fixup_any(u8 *rec, u32 recsz)
{
    u32 usa_off = le16(rec + 4), usa_cnt = le16(rec + 6), i;
    if (usa_off + 2 * usa_cnt > recsz) return -1;
    for (i = 1; i < usa_cnt; i++) {
        u32 p = i * 512 - 2;
        if (p + 2 > recsz) break;
        if (rec[p] != rec[usa_off] || rec[p + 1] != rec[usa_off + 1]) return -1;
        rec[p] = rec[usa_off + 2 * i];
        rec[p + 1] = rec[usa_off + 2 * i + 1];
    }
    return 0;
}

/* reaplica a protecao antes de gravar o registro */
static void ntfs_protect(u8 *rec, u32 recsz)
{
    u32 usa_off = le16(rec + 4), usa_cnt = le16(rec + 6), i;
    for (i = 1; i < usa_cnt; i++) {
        u32 p = i * 512 - 2;
        if (p + 2 > recsz) break;
        rec[usa_off + 2 * i] = rec[p];
        rec[usa_off + 2 * i + 1] = rec[p + 1];
        rec[p] = rec[usa_off];
        rec[p + 1] = rec[usa_off + 1];
    }
}

static int ntfs_fixup(u8 *rec, u32 recsz)
{
    if (memcmp(rec, "FILE", 4) != 0) return -1;
    return ntfs_fixup_any(rec, recsz);
}

static int ntfs_read_rec(Dev *d, u64 off, const NtfsGeo *g, u32 n, u8 *rec)
{
    if (dev_read_any(d, off + g->mft_lcn * g->cs + (u64)n * g->recsz, g->recsz, rec) != 0) return -1;
    return ntfs_fixup(rec, g->recsz);
}

/* procura atributo; name = NULL para atributo sem nome. retorna offset ou -1 */
static int ntfs_find_attr(const u8 *rec, u32 recsz, u32 type, const char *name)
{
    u32 a = le16(rec + 0x14);
    while (a + 16 <= recsz) {
        u32 t = le32(rec + a), alen = le32(rec + a + 4);
        if (t == 0xFFFFFFFF || alen < 16 || a + alen > recsz) return -1;
        if (t == type) {
            u32 nl = rec[a + 9], no = le16(rec + a + 10), k;
            if (!name && nl == 0) return (int)a;
            if (name && nl == strlen(name) && a + no + 2 * nl <= recsz) {
                for (k = 0; k < nl; k++)
                    if (le16(rec + a + no + 2 * k) != (u8)name[k]) break;
                if (k == nl) return (int)a;
            }
        }
        a += alen;
    }
    return -1;
}

/* decodifica a runlist de um atributo nao residente */
static int ntfs_runs(const u8 *rec, u32 a, Run *runs, int max)
{
    u32 alen = le32(rec + a + 4), p = a + le16(rec + a + 0x20);
    i64 lcn = 0;
    int n = 0;
    while (p < a + alen && rec[p]) {
        u8 h = rec[p++];
        u32 ls = h & 15, os = h >> 4, k;
        u64 rl = 0;
        if (ls == 0 || ls > 8 || os > 8 || p + ls + os > a + alen || n >= max) return -1;
        for (k = 0; k < ls; k++) rl |= (u64)rec[p + k] << (8 * k);
        p += ls;
        runs[n].len = rl;
        if (os) {
            u64 v = 0;
            for (k = 0; k < os; k++) v |= (u64)rec[p + k] << (8 * k);
            if (os < 8 && (rec[p + os - 1] & 0x80)) v |= ~0ull << (8 * os);
            lcn += (i64)v;
            p += os;
            runs[n].lcn = lcn;
        } else {
            runs[n].lcn = -1;
        }
        n++;
    }
    return n;
}

#define MAX_RUNS 4096

/* le o $Bitmap (clusters usados). Retorna NULL se nao conseguir. */
static u8 *ntfs_load_bitmap(Dev *d, u64 off, const NtfsGeo *g)
{
    u8 *rec = malloc(g->recsz), *bm = NULL;
    Run *runs = malloc(sizeof(Run) * MAX_RUNS);
    u64 bmbytes = (g->ncl + 7) / 8, pos = 0;
    int a, n, i;
    if (!rec || !runs || ntfs_read_rec(d, off, g, 6, rec) != 0) goto fail;
    a = ntfs_find_attr(rec, g->recsz, 0x80, NULL);
    if (a < 0) goto fail;
    bm = calloc(1, (size_t)bmbytes + 8);
    if (!bm) goto fail;
    if (rec[a + 8] == 0) { /* residente */
        u32 vlen = le32(rec + a + 0x10), voff = le16(rec + a + 0x14);
        if (a + voff + vlen > g->recsz) goto fail;
        memcpy(bm, rec + a + voff, vlen < bmbytes ? vlen : (size_t)bmbytes);
        pos = vlen;
    } else {
        n = ntfs_runs(rec, (u32)a, runs, MAX_RUNS);
        if (n < 0) goto fail;
        for (i = 0; i < n; i++) {
            u64 bytes = runs[i].len * g->cs;
            if (runs[i].lcn >= 0 && pos < bmbytes) {
                u64 want = bytes < bmbytes - pos ? bytes : bmbytes - pos;
                if (dev_read_any(d, off + (u64)runs[i].lcn * g->cs, want, bm + pos) != 0) goto fail;
            }
            pos += bytes;
        }
    }
    if (pos < bmbytes) goto fail;
    /* sanidade: boot e MFT precisam constar como usados */
    if (!(bm[0] & 1) || !((bm[g->mft_lcn >> 3] >> (g->mft_lcn & 7)) & 1)) goto fail;
    free(rec);
    free(runs);
    return bm;
fail:
    free(rec);
    free(runs);
    free(bm);
    return NULL;
}
