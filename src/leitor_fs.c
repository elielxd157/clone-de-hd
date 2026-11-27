/* hdclone - leitor NTFS, exclusoes, exFAT, ext2/3/4, particoes */

/* ------------------------------------------------------------------ */
/* leitor NTFS somente leitura (pastas e arquivos)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    Run *r;
    int n, cap;
} RunList;

typedef struct {
    Dev *d;
    u64 off;      /* inicio da particao no disco */
    NtfsGeo g;
    RunList mft;  /* runlist do $MFT */
} Ntfs;

typedef struct {
    int resident;
    u16 flags;    /* 0x0001 comprimido, 0x4000 criptografado, 0x8000 esparso */
    u64 size, init_size;
    u8 *res;      /* dados (atributo residente) */
    u32 res_len;
    RunList rl;
} AttrData;

static void rl_free(RunList *l)
{
    free(l->r);
    memset(l, 0, sizeof(*l));
}

static int rl_append(RunList *l, const u8 *rec, u32 a)
{
    Run *tmp = malloc(sizeof(Run) * MAX_RUNS);
    int n, i;
    if (!tmp) return -1;
    n = ntfs_runs(rec, a, tmp, MAX_RUNS);
    if (n < 0) { free(tmp); return -1; }
    if (l->n + n > l->cap) {
        int nc = (l->n + n) * 2 + 16;
        Run *r = realloc(l->r, sizeof(Run) * nc);
        if (!r) { free(tmp); return -1; }
        l->r = r;
        l->cap = nc;
    }
    for (i = 0; i < n; i++) l->r[l->n++] = tmp[i];
    free(tmp);
    return 0;
}

static void ad_free(AttrData *ad)
{
    free(ad->res);
    rl_free(&ad->rl);
    memset(ad, 0, sizeof(*ad));
}

/* le bytes de um atributo nao residente (faixas esparsas viram zero) */
static int nt_read_rl(Ntfs *nt, const RunList *l, u64 off, u64 len, u8 *out)
{
    u64 cs = nt->g.cs, vcn0 = 0;
    int i = 0;
    while (len) {
        u64 vcn = off / cs, end, n;
        while (i < l->n && vcn >= vcn0 + l->r[i].len) { vcn0 += l->r[i].len; i++; }
        if (i >= l->n) { memset(out, 0, (size_t)len); return 0; }
        end = (vcn0 + l->r[i].len) * cs;
        n = end - off < len ? end - off : len;
        if (l->r[i].lcn < 0) memset(out, 0, (size_t)n);
        else if (dev_read_any(nt->d, nt->off + (u64)l->r[i].lcn * cs + (off - vcn0 * cs), n, out) != 0)
            return -1;
        out += n; off += n; len -= n;
    }
    return 0;
}

static int nt_rec(Ntfs *nt, u64 recno, u8 *buf)
{
    if (nt_read_rl(nt, &nt->mft, recno * nt->g.recsz, nt->g.recsz, buf) != 0) return -1;
    if (ntfs_fixup(buf, nt->g.recsz) != 0) return -1;
    return (le16(buf + 0x16) & 1) ? 0 : -1; /* registro em uso? */
}

/* nome do atributo confere? (name NULL = sem nome) */
static int attr_name_eq(const u8 *nm, int nl, const char *name)
{
    int k;
    if (!name) return nl == 0;
    if (nl != (int)strlen(name)) return 0;
    for (k = 0; k < nl; k++)
        if (le16(nm + 2 * k) != (u8)name[k]) return 0;
    return 1;
}

/* acha atributo num registro; svcn = VCN inicial exigido (ou -1 para qualquer) */
static int nt_find(const u8 *rec, u32 recsz, u32 type, const char *name, i64 svcn)
{
    u32 a = le16(rec + 0x14);
    while (a + 16 <= recsz) {
        u32 t = le32(rec + a), alen = le32(rec + a + 4);
        if (t == 0xFFFFFFFF || alen < 16 || a + alen > recsz) return -1;
        if (t == type && a + le16(rec + a + 10) + 2u * rec[a + 9] <= recsz &&
            attr_name_eq(rec + a + le16(rec + a + 10), rec[a + 9], name) &&
            (svcn < 0 || rec[a + 8] == 0 || (i64)le64(rec + a + 0x10) == svcn))
            return (int)a;
        a += alen;
    }
    return -1;
}

static int ad_take(AttrData *ad, const u8 *rec, u32 a)
{
    if (rec[a + 8] == 0) {
        u32 vlen = le32(rec + a + 0x10), voff = le16(rec + a + 0x14);
        ad->resident = 1;
        ad->res = malloc(vlen + 1);
        if (!ad->res) return -1;
        memcpy(ad->res, rec + a + voff, vlen);
        ad->res_len = vlen;
        ad->size = ad->init_size = vlen;
        ad->flags = le16(rec + a + 0x0C);
        return 0;
    }
    if (le64(rec + a + 0x10) == 0) { /* primeiro trecho: tamanhos validos */
        ad->size = le64(rec + a + 0x30);
        ad->init_size = le64(rec + a + 0x38);
        ad->flags = le16(rec + a + 0x0C);
    }
    return rl_append(&ad->rl, rec, a);
}

/* reune um atributo completo, inclusive espalhado em varios registros
   (lista de atributos, comum em arquivos grandes e fragmentados) */
static int nt_get_attr(Ntfs *nt, u64 recno, u32 type, const char *name, AttrData *ad)
{
    u32 rs = nt->g.recsz;
    u8 *base = malloc(rs), *ext = malloc(rs), *al = NULL;
    u64 allen = 0, p;
    int a, rc = -1, found = 0;
    memset(ad, 0, sizeof(*ad));
    if (!base || !ext || nt_rec(nt, recno, base) != 0) goto out;
    a = nt_find(base, rs, 0x20, NULL, -1);
    if (a < 0) {
        a = nt_find(base, rs, type, name, -1);
        if (a >= 0 && ad_take(ad, base, (u32)a) == 0) rc = 0;
        goto out;
    }
    /* carrega a lista de atributos */
    if (base[a + 8] == 0) {
        allen = le32(base + a + 0x10);
        al = malloc((size_t)allen + 1);
        if (!al) goto out;
        memcpy(al, base + a + le16(base + a + 0x14), (size_t)allen);
    } else {
        RunList l;
        memset(&l, 0, sizeof(l));
        allen = le64(base + a + 0x30);
        if (allen > 16 * 1024 * 1024 || rl_append(&l, base, (u32)a) != 0) { rl_free(&l); goto out; }
        al = malloc((size_t)allen + 1);
        if (!al || nt_read_rl(nt, &l, 0, allen, al) != 0) { rl_free(&l); goto out; }
        rl_free(&l);
    }
    for (p = 0; p + 0x1A <= allen;) {
        u32 et = le32(al + p), el = le16(al + p + 4);
        if (el < 0x1A || p + el > allen) break;
        if (et == type && p + al[p + 7] + 2u * al[p + 6] <= allen &&
            attr_name_eq(al + p + al[p + 7], al[p + 6], name)) {
            u64 ref = le64(al + p + 0x10) & 0xFFFFFFFFFFFFull;
            i64 svcn = (i64)le64(al + p + 8);
            const u8 *r = base;
            int b;
            if (ref != recno) {
                if (nt_rec(nt, ref, ext) != 0) goto out;
                r = ext;
            }
            b = nt_find(r, rs, type, name, svcn);
            if (b < 0 || ad_take(ad, r, (u32)b) != 0) goto out;
            found = 1;
            if (ad->resident) break;
        }
        p += el;
    }
    if (found) rc = 0;
out:
    if (rc != 0) ad_free(ad);
    free(base);
    free(ext);
    free(al);
    return rc;
}

/* le os dados de um atributo inteiro para a memoria (para atributos pequenos) */
static u8 *nt_attr_bytes(Ntfs *nt, AttrData *ad, u64 *len)
{
    u8 *b;
    *len = ad->size;
    if (ad->resident) return ad->res;
    if (ad->size > 64 * 1024 * 1024) return NULL;
    b = malloc((size_t)ad->size + 1);
    if (!b || nt_read_rl(nt, &ad->rl, 0, ad->size, b) != 0) { free(b); return NULL; }
    free(ad->res);
    ad->res = b;
    return b;
}

static int nt_open(Ntfs *nt, Dev *d, u64 off, u64 plen)
{
    u8 b[512], *rec;
    int a;
    memset(nt, 0, sizeof(*nt));
    nt->d = d;
    nt->off = off;
    if (dev_read_any(d, off, 512, b) != 0 || memcmp(b + 3, "NTFS    ", 8) != 0 || ntfs_geo(b, plen, &nt->g) != 0)
        return -1;
    rec = malloc(nt->g.recsz);
    if (!rec || ntfs_read_rec(d, off, &nt->g, 0, rec) != 0 || (a = nt_find(rec, nt->g.recsz, 0x80, NULL, 0)) < 0 ||
        rec[a + 8] == 0 || rl_append(&nt->mft, rec, (u32)a) != 0) {
        free(rec);
        rl_free(&nt->mft);
        return -1;
    }
    if (nt_find(rec, nt->g.recsz, 0x20, NULL, -1) >= 0) { /* $MFT fragmentado em varios registros */
        AttrData ad;
        if (nt_get_attr(nt, 0, 0x80, NULL, &ad) == 0 && !ad.resident) {
            rl_free(&nt->mft);
            nt->mft = ad.rl;
            ad.rl.r = NULL;
        }
        ad_free(&ad);
    }
    free(rec);
    return 0;
}

static void nt_close(Ntfs *nt) { rl_free(&nt->mft); }

/* ---- pastas ---- */

typedef struct {
    u64 rec;
    u64 size;
    u64 mtime;  /* FILETIME (100 ns desde 1601) */
    u32 attr;
    int is_dir;
    int nlen;
    u16 name[256];
} DirEnt;

typedef struct {
    DirEnt *e;
    int n, cap;
} DirList;

#define FA_REPARSE 0x400
#define FA_DIR_IDX 0x10000000

static void dl_free(DirList *dl)
{
    free(dl->e);
    memset(dl, 0, sizeof(*dl));
}

static u16 fold16(u16 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) return (u16)(c - 0x20);
    return c;
}

static int name_cmp(const u16 *a, int na, const u16 *b, int nb)
{
    int i;
    for (i = 0; i < na && i < nb; i++) {
        u16 x = fold16(a[i]), y = fold16(b[i]);
        if (x != y) return x < y ? -1 : 1;
    }
    return na - nb;
}

static int name_eq_ascii(const DirEnt *e, const char *s)
{
    int i, n = (int)strlen(s);
    if (e->nlen != n) return 0;
    for (i = 0; i < n; i++)
        if (fold16(e->name[i]) != fold16((u8)s[i])) return 0;
    return 1;
}

static void dl_parse(DirList *dl, const u8 *buf, u64 len)
{
    u64 p = 0;
    while (p + 16 <= len) {
        u32 elen = le16(buf + p + 8), klen = le16(buf + p + 10), fl = le16(buf + p + 12);
        const u8 *k = buf + p + 16;
        if (elen < 16 || p + elen > len || (fl & 2)) break;
        if (klen >= 0x42 && 16 + klen <= elen && k[0x41] != 2 && 0x42 + 2u * k[0x40] <= klen) {
            DirEnt *d;
            int i;
            if (dl->n == dl->cap) {
                int nc = dl->cap ? dl->cap * 2 : 64;
                DirEnt *ne = realloc(dl->e, sizeof(DirEnt) * nc);
                if (!ne) return;
                dl->e = ne;
                dl->cap = nc;
            }
            d = &dl->e[dl->n];
            d->rec = le64(buf + p) & 0xFFFFFFFFFFFFull;
            d->mtime = le64(k + 0x10);
            d->size = le64(k + 0x30);
            d->attr = le32(k + 0x38);
            d->is_dir = (d->attr & FA_DIR_IDX) != 0;
            d->nlen = k[0x40];
            for (i = 0; i < d->nlen; i++) d->name[i] = le16(k + 0x42 + 2 * i);
            dl->n++;
        }
        p += elen;
    }
}

static int dirent_order(const void *x, const void *y)
{
    const DirEnt *a = x, *b = y;
    if (a->is_dir != b->is_dir) return b->is_dir - a->is_dir;
    return name_cmp(a->name, a->nlen, b->name, b->nlen);
}

/* lista uma pasta (registro da MFT); arquivos de sistema e "." ficam de fora */
static int nt_list(Ntfs *nt, u64 recno, DirList *dl)
{
    AttrData root, alloc, bmp;
    u8 *v, *buf = NULL, *bits = NULL;
    u64 bitlen = 0, nb, i;
    u32 ibs, eo, tot;
    int j, k;
    memset(dl, 0, sizeof(*dl));
    if (nt_get_attr(nt, recno, 0x90, "$I30", &root) != 0 || !root.resident || root.res_len < 32) {
        ad_free(&root);
        return -1;
    }
    v = root.res;
    ibs = le32(v + 8);
    eo = le32(v + 16);
    tot = le32(v + 20);
    if (16 + tot <= root.res_len && eo < tot) dl_parse(dl, v + 16 + eo, tot - eo);
    ad_free(&root);
    if (ibs >= 512 && ibs <= 65536 && nt_get_attr(nt, recno, 0xA0, "$I30", &alloc) == 0) {
        if (!alloc.resident) {
            int have = nt_get_attr(nt, recno, 0xB0, "$I30", &bmp) == 0;
            if (have) bits = nt_attr_bytes(nt, &bmp, &bitlen);
            buf = malloc(ibs);
            nb = alloc.size / ibs;
            for (i = 0; buf && i < nb; i++) {
                const u8 *ih;
                if (bits && (i / 8 >= bitlen || !((bits[i / 8] >> (i & 7)) & 1))) continue;
                if (nt_read_rl(nt, &alloc.rl, i * ibs, ibs, buf) != 0) continue;
                if (memcmp(buf, "INDX", 4) != 0 || ntfs_fixup_any(buf, ibs) != 0) continue;
                ih = buf + 0x18;
                eo = le32(ih);
                tot = le32(ih + 4);
                if (0x18 + tot <= ibs && eo < tot) dl_parse(dl, ih + eo, tot - eo);
            }
            free(buf);
            if (have) ad_free(&bmp);
        }
        ad_free(&alloc);
    }
    /* filtra metadados do NTFS ($MFT, $Bitmap...) e "." */
    for (j = k = 0; j < dl->n; j++) {
        DirEnt *e = &dl->e[j];
        if (e->rec < 16 || (e->nlen == 1 && e->name[0] == '.')) continue;
        dl->e[k++] = *e;
    }
    dl->n = k;
    qsort(dl->e, dl->n, sizeof(DirEnt), dirent_order);
    /* remove repetidos (mesmo registro e mesmo nome) */
    for (j = k = 0; j < dl->n; j++) {
        if (k && dl->e[k - 1].rec == dl->e[j].rec &&
            name_cmp(dl->e[k - 1].name, dl->e[k - 1].nlen, dl->e[j].name, dl->e[j].nlen) == 0)
            continue;
        dl->e[k++] = dl->e[j];
    }
    dl->n = k;
    return 0;
}

/* ---- pagefile.sys / hiberfil.sys / swapfile.sys ---- */

/* faixas que devem ser gravadas como zero na imagem (inicio do hiberfil etc.) */
#define MAX_ZR 64
static struct { u64 off, len; } g_zr[MAX_ZR];
static int g_keep_sys; /* --keep-sys: copiar esses arquivos mesmo assim */

static void exclude_sysfiles(Dev *d, u64 off, u64 plen, u8 *bm, u64 ncl)
{
    static const char *names[] = {"pagefile.sys", "hiberfil.sys", "swapfile.sys"};
    Ntfs nt;
    DirList dl;
    int i, k, r;
    if (nt_open(&nt, d, off, plen) != 0) return;
    if (nt_list(&nt, 5, &dl) != 0) { nt_close(&nt); return; }
    for (i = 0; i < dl.n; i++) {
        DirEnt *e = &dl.e[i];
        AttrData ad;
        u64 vcn = 0, freed = 0, keep = (4096 + nt.g.cs - 1) / nt.g.cs; /* 1a pagina: zerada */
        char sz[32];
        for (k = 0; k < 3 && !name_eq_ascii(e, names[k]); k++)
            ;
        if (k == 3 || e->is_dir) continue;
        if (nt_get_attr(&nt, e->rec, 0x80, NULL, &ad) != 0) continue;
        if (ad.resident) { ad_free(&ad); continue; }
        for (r = 0; r < ad.rl.n; r++) {
            Run *ru = &ad.rl.r[r];
            u64 c;
            for (c = 0; c < ru->len; c++, vcn++) {
                u64 lcn;
                if (ru->lcn < 0) continue;
                lcn = (u64)ru->lcn + c;
                if (lcn >= ncl) continue;
                if (vcn < keep) {
                    if (g_nzr < MAX_ZR) {
                        g_zr[g_nzr].off = off + lcn * nt.g.cs;
                        g_zr[g_nzr].len = nt.g.cs;
                        g_nzr++;
                    }
                    continue;
                }
                if (bm[lcn >> 3] & (1 << (lcn & 7))) {
                    bm[lcn >> 3] &= (u8)~(1 << (lcn & 7));
                    freed++;
                }
            }
        }
        fmt_size(freed * nt.g.cs, sz);
        printf("    %s: %s nao serao copiados (o Windows recria o arquivo)\n", names[k], sz);
        ad_free(&ad);
    }
    dl_free(&dl);
    nt_close(&nt);
}

/* ---- pontos de restauracao (Vista+) e despejos de memoria: gravados como zeros ----
   Os arquivos continuam existindo (o NTFS fica integro), mas com conteudo zerado:
   o Windows descarta as copias de sombra invalidas e um despejo vazio so e inutil.
   No XP os pontos de restauracao sao copias de arquivos de verdade e NAO sao tocados. */
static int g_skip_restore;

static int name_ends(const DirEnt *e, const char *suf)
{
    int n = (int)strlen(suf), i;
    if (e->nlen < n) return 0;
    for (i = 0; i < n; i++)
        if (fold16(e->name[e->nlen - n + i]) != fold16((u8)suf[i])) return 0;
    return 1;
}

static u64 ntfs_mark_zero(Ntfs *nt, u64 rec, u8 **zb, u64 ncl)
{
    AttrData ad;
    u64 n = 0, c;
    int r;
    if (nt_get_attr(nt, rec, 0x80, NULL, &ad) != 0) return 0;
    if (!ad.resident && !(ad.flags & 0x4001)) { /* nao mexe em comprimidos/criptografados */
        if (!*zb) *zb = calloc(1, (size_t)(ncl + 7) / 8 + 8);
        if (*zb)
            for (r = 0; r < ad.rl.n; r++) {
                if (ad.rl.r[r].lcn < 0) continue;
                for (c = 0; c < ad.rl.r[r].len; c++) {
                    u64 l = (u64)ad.rl.r[r].lcn + c;
                    if (l < ncl) { (*zb)[l >> 3] |= (u8)(1 << (l & 7)); n++; }
                }
            }
    }
    ad_free(&ad);
    return n;
}

/* marca arquivos de uma pasta: sufixo no nome (ou NULL = todos com extensao ext) */
static u64 mark_in_dir(Ntfs *nt, u64 dir, const char *suffix, u8 **zb, u64 ncl, int depth)
{
    DirList dl;
    u64 n = 0;
    int i;
    if (depth > 2 || nt_list(nt, dir, &dl) != 0) return 0;
    for (i = 0; i < dl.n; i++) {
        DirEnt *e = &dl.e[i];
        if (e->is_dir) {
            if (depth < 1 && !(e->attr & FA_REPARSE)) n += mark_in_dir(nt, e->rec, suffix, zb, ncl, depth + 1);
        } else if (name_ends(e, suffix)) {
            n += ntfs_mark_zero(nt, e->rec, zb, ncl);
        }
    }
    dl_free(&dl);
    return n;
}

static void exclude_restore(Dev *d, u64 off, u64 plen, u8 **zb, u64 ncl)
{
    Ntfs nt;
    DirList dl;
    int i;
    u64 vss = 0, dmp = 0;
    char s1[32], s2[32];
    if (nt_open(&nt, d, off, plen) != 0) return;
    if (nt_list(&nt, 5, &dl) == 0) {
        for (i = 0; i < dl.n; i++) {
            DirEnt *e = &dl.e[i];
            if (!e->is_dir) continue;
            if (name_eq_ascii(e, "System Volume Information"))
                vss += mark_in_dir(&nt, e->rec, "{3808876b-c176-4e48-b7ae-04046e6cc752}", zb, ncl, 1);
            else if (name_eq_ascii(e, "Windows")) {
                DirList w;
                int j;
                if (nt_list(&nt, e->rec, &w) == 0) {
                    for (j = 0; j < w.n; j++) {
                        DirEnt *f = &w.e[j];
                        if (!f->is_dir && name_eq_ascii(f, "MEMORY.DMP")) dmp += ntfs_mark_zero(&nt, f->rec, zb, ncl);
                        else if (f->is_dir && (name_eq_ascii(f, "Minidump") || name_eq_ascii(f, "LiveKernelReports")))
                            dmp += mark_in_dir(&nt, f->rec, ".dmp", zb, ncl, 0);
                    }
                    dl_free(&w);
                }
            }
        }
        dl_free(&dl);
    }
    if (vss || dmp) {
        fmt_size(vss * nt.g.cs, s1);
        fmt_size(dmp * nt.g.cs, s2);
        printf("    pontos de restauracao: %s; despejos de memoria: %s (gravados como zeros)\n", s1, s2);
    }
    nt_close(&nt);
}

static void map_ntfs(Dev *d, u64 off, u64 plen, const u8 *b)
{
    NtfsGeo g;
    u8 *bm;
    char s1[32], s2[32];
    if (ntfs_geo(b, plen, &g) != 0 || !(bm = ntfs_load_bitmap(d, off, &g))) {
        printf("  NTFS em %llu MB: nao foi possivel ler o bitmap, sera copiado inteiro\n",
               (unsigned long long)(off >> 20));
        return;
    }
    fmt_size(g.ncl * g.cs, s1);
    fmt_size(count_used(bm, g.ncl) * g.cs, s2);
    printf("  NTFS em %llu MB: %s, usado %s (cluster %u bytes)\n",
           (unsigned long long)(off >> 20), s1, s2, g.cs);
    if (!g_keep_sys) exclude_sysfiles(d, off, plen, bm, g.ncl);
    add_area(off, g.cs, g.ncl, bm);
    if (g_skip_restore && g_nareas && g_areas[g_nareas - 1].bm == bm)
        exclude_restore(d, off, plen, &g_areas[g_nareas - 1].zb, g.ncl);
}

/* ---- FAT32 ---- */

typedef struct {
    u32 bps, spc, rsv, nfats, tot, fatsz, root;
    u64 data_sec, ncl, cs;
} FatGeo;

static int fat_geo(const u8 *b, u64 plen, FatGeo *g)
{
    memset(g, 0, sizeof(*g));
    g->bps = le16(b + 11);
    g->spc = b[13];
    g->rsv = le16(b + 14);
    g->nfats = b[16];
    g->tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    g->fatsz = le32(b + 36);
    g->root = le32(b + 44);
    if (g->bps < 512 || g->bps > 4096 || !g->spc || !g->nfats || !g->fatsz) return -1;
    g->data_sec = g->rsv + (u64)g->nfats * g->fatsz;
    if (g->tot <= g->data_sec) return -1;
    g->ncl = (g->tot - g->data_sec) / g->spc;
    g->cs = (u64)g->bps * g->spc;
    if ((u64)g->tot * g->bps > plen || (g->ncl + 2) * 4 > (u64)g->fatsz * g->bps) return -1;
    return 0;
}

/* le a FAT e monta o mapa de clusters usados (bit i = cluster i+2) */
static u8 *fat_load_bitmap(Dev *d, u64 off, const FatGeo *g)
{
    u8 *bm = calloc(1, (size_t)(g->ncl + 7) / 8 + 8), *buf = malloc(CHUNK_SIZE);
    u64 done = 0;
    if (!bm || !buf) goto fail;
    while (done < g->ncl) {
        u64 n = g->ncl - done, k;
        if (n > CHUNK_SIZE / 4) n = CHUNK_SIZE / 4;
        if (dev_read_any(d, off + (u64)g->rsv * g->bps + (done + 2) * 4, n * 4, buf) != 0) goto fail;
        for (k = 0; k < n; k++)
            if (le32(buf + k * 4) & 0x0FFFFFFF) bm[(done + k) >> 3] |= (u8)(1 << ((done + k) & 7));
        done += n;
    }
    if (g->root < 2 || g->root - 2 >= g->ncl || !bm_get(bm, g->root - 2)) goto fail;
    free(buf);
    return bm;
fail:
    free(bm);
    free(buf);
    return NULL;
}

static void map_fat32(Dev *d, u64 off, u64 plen, const u8 *b)
{
    FatGeo g;
    u8 *bm;
    char s1[32], s2[32];
    if (fat_geo(b, plen, &g) != 0 || !(bm = fat_load_bitmap(d, off, &g))) {
        printf("  FAT32 em %llu MB: nao foi possivel ler a FAT, sera copiado inteiro\n",
               (unsigned long long)(off >> 20));
        return;
    }
    fmt_size(g.ncl * g.cs, s1);
    fmt_size(count_used(bm, g.ncl) * g.cs, s2);
    printf("  FAT32 em %llu MB: %s, usado %s (cluster %llu bytes)\n",
           (unsigned long long)(off >> 20), s1, s2, (unsigned long long)g.cs);
    add_area(off + g.data_sec * g.bps, g.cs, g.ncl, bm);
}

/* ---- exFAT (pendrives e HDs externos) ---- */
static void map_exfat(Dev *d, u64 off, u64 plen, const u8 *b)
{
    u32 bshift = b[108], cshift = b[109], fat_off = le32(b + 80), heap = le32(b + 88), ncl = le32(b + 92),
        root = le32(b + 96), cl, guard = 0;
    u64 bps, cs, vol = le64(b + 72), bmfirst = 0, bmlen = 0, done = 0;
    u8 *buf = NULL, *bm = NULL;
    char s1[32], s2[32];
#define EX_NEXT(c) (dev_read_any(d, off + (u64)fat_off * bps + (u64)(c) * 4, 4, nb) == 0 ? le32(nb) : 0xFFFFFFFFu)
    u8 nb[4];
    if (bshift < 9 || bshift > 12 || cshift > 25 - bshift) goto fail;
    bps = 1ull << bshift;
    cs = bps << cshift;
    if (vol * bps > plen || (u64)heap * bps + (u64)ncl * cs > vol * bps || ncl < 1) goto fail;
    buf = malloc((size_t)cs);
    if (!buf) goto fail;
    /* procura a entrada do bitmap de alocacao (tipo 0x81) na pasta raiz */
    for (cl = root; cl >= 2 && cl < ncl + 2 && guard++ < 4096 && !bmlen; cl = EX_NEXT(cl)) {
        u64 i;
        if (dev_read_any(d, off + (u64)heap * bps + (u64)(cl - 2) * cs, cs, buf) != 0) goto fail;
        for (i = 0; i + 32 <= cs; i += 32) {
            if (buf[i] == 0x00) break;
            if (buf[i] == 0x81 && !(buf[i + 1] & 1)) { bmfirst = le32(buf + i + 20); bmlen = le64(buf + i + 24); break; }
        }
    }
    if (!bmlen || bmlen < (ncl + 7) / 8 || bmfirst < 2) goto fail;
    bm = calloc(1, (size_t)bmlen + 8);
    if (!bm) goto fail;
    guard = 0;
    for (cl = (u32)bmfirst; done < bmlen && cl >= 2 && cl < ncl + 2 && guard++ <= ncl; cl = EX_NEXT(cl)) {
        u64 n = bmlen - done < cs ? bmlen - done : cs;
        if (dev_read_any(d, off + (u64)heap * bps + (u64)(cl - 2) * cs, n, bm + done) != 0) goto fail;
        done += n;
    }
#undef EX_NEXT
    if (done < bmlen || !bm_get(bm, root - 2) || !bm_get(bm, bmfirst - 2)) goto fail;
    free(buf);
    fmt_size((u64)ncl * cs, s1);
    fmt_size(count_used(bm, ncl) * cs, s2);
    printf("  exFAT em %llu MB: %s, usado %s (cluster %llu bytes)\n", (unsigned long long)(off >> 20), s1, s2,
           (unsigned long long)cs);
    add_area(off + (u64)heap * bps, cs, ncl, bm);
    return;
fail:
    printf("  exFAT em %llu MB: nao foi possivel ler o bitmap, sera copiado inteiro\n", (unsigned long long)(off >> 20));
    free(buf);
    free(bm);
}

/* ---- ext2/3/4 (Linux) ----
   Grupos "nao inicializados" (BLOCK_UNINIT) nao tem bitmap gravado: neles, como faz
   o proprio kernel, so os metadados (copias do superbloco/descritores, bitmaps e
   tabelas de inodes) contam como usados. Formatos incomuns (meta_bg): copia inteiro. */
static int ext_has_super(u32 g, u32 compat, u32 rocompat, const u8 *sb)
{
    u32 p;
    if (compat & 0x200) return g == 0 || g == le32(sb + 0x24C) || g == le32(sb + 0x250); /* sparse_super2 */
    if (!(rocompat & 0x1) || g <= 1) return 1;
    for (p = 3; p <= g; p *= 3) if (p == g) return 1;
    for (p = 5; p <= g; p *= 5) if (p == g) return 1;
    for (p = 7; p <= g; p *= 7) if (p == g) return 1;
    return 0;
}

static void map_ext(Dev *d, u64 off, u64 plen)
{
    u8 sb[1024], *gdt = NULL, *bm = NULL, *blk = NULL;
    u32 log, first, bpg, ipg, isz, compat, incompat, rocompat, dsz, g, ng, gdtb, rgdt, itb;
    u64 bs, blocks, i;
    int uninit_ok;
    char s1[32], s2[32];
    if (dev_read_any(d, off + 1024, 1024, sb) != 0 || le16(sb + 56) != 0xEF53) return;
    log = le32(sb + 24);
    compat = le32(sb + 92);
    incompat = le32(sb + 96);
    rocompat = le32(sb + 100);
    if (log > 6) goto fail;
    bs = 1024ull << log;
    blocks = le32(sb + 4) | ((incompat & 0x80) ? (u64)le32(sb + 0x150) << 32 : 0);
    first = le32(sb + 20);
    bpg = le32(sb + 32);
    ipg = le32(sb + 40);
    isz = le32(sb + 76) >= 1 ? le16(sb + 88) : 128;
    dsz = (incompat & 0x80) ? le16(sb + 254) : 32;
    if (dsz < 32) dsz = 32;
    if ((incompat & 0x10) || !bpg || bpg > bs * 8 || !isz || blocks * bs > plen || blocks <= first) goto fail;
    ng = (u32)((blocks - first + bpg - 1) / bpg);
    gdtb = (u32)(((u64)ng * dsz + bs - 1) / bs);
    rgdt = le16(sb + 206);
    itb = (u32)(((u64)ipg * isz + bs - 1) / bs);
    uninit_ok = (rocompat & (0x10 | 0x400)) != 0; /* gdt_csum / metadata_csum: flags de grupo valem */
    gdt = malloc((size_t)gdtb * bs);
    bm = calloc(1, (size_t)(blocks + 7) / 8 + 8);
    blk = malloc((size_t)bs);
    if (!gdt || !bm || !blk) goto fail;
    if (dev_read_any(d, off + (u64)(first + 1) * bs, (u64)gdtb * bs, gdt) != 0) goto fail;
#define MARK(b0, n)                                                       \
    do {                                                                  \
        u64 _b = (b0), _e = (b0) + (n);                                   \
        if (_e > blocks) _e = blocks;                                     \
        for (; _b < _e; _b++) bm[_b >> 3] |= (u8)(1 << (_b & 7));         \
    } while (0)
    MARK(0, (u64)first + 1); /* setor de boot / superbloco */
    for (g = 0; g < ng; g++) {
        const u8 *de = gdt + (size_t)g * dsz;
        u64 bb = le32(de) | (dsz >= 64 ? (u64)le32(de + 0x20) << 32 : 0);
        u64 ib = le32(de + 4) | (dsz >= 64 ? (u64)le32(de + 0x24) << 32 : 0);
        u64 it = le32(de + 8) | (dsz >= 64 ? (u64)le32(de + 0x28) << 32 : 0);
        u64 gs = first + (u64)g * bpg, gn = blocks - gs < bpg ? blocks - gs : bpg;
        if (bb >= blocks || ib >= blocks || it >= blocks) goto fail;
        if (uninit_ok && (le16(de + 18) & 0x2)) { /* BLOCK_UNINIT */
            if (ext_has_super(g, compat, rocompat, sb)) MARK(gs, 1 + (u64)gdtb + rgdt);
        } else {
            if (dev_read_any(d, off + bb * bs, bs, blk) != 0) goto fail;
            for (i = 0; i < gn; i++)
                if (bm_get(blk, i)) bm[(gs + i) >> 3] |= (u8)(1 << ((gs + i) & 7));
        }
        MARK(bb, 1);
        MARK(ib, 1);
        MARK(it, itb);
    }
#undef MARK
    free(gdt);
    free(blk);
    fmt_size(blocks * bs, s1);
    fmt_size(count_used(bm, blocks) * bs, s2);
    printf("  ext2/3/4 (Linux) em %llu MB: %s, usado %s (bloco %llu bytes)\n", (unsigned long long)(off >> 20), s1, s2,
           (unsigned long long)bs);
    add_area(off, bs, blocks, bm);
    return;
fail:
    printf("  ext2/3/4 em %llu MB: formato nao suportado para copia inteligente, sera copiado inteiro\n",
           (unsigned long long)(off >> 20));
    free(gdt);
    free(bm);
    free(blk);
}

static void probe_fs(Dev *d, u64 off, u64 len)
{
    u8 b[512], e[2];
    if (off + 2048 <= d->size && dev_read_any(d, off + 1024 + 56, 2, e) == 0 && le16(e) == 0xEF53) {
        map_ext(d, off, len);
        return;
    }
    if (off + 512 > d->size || dev_read_any(d, off, 512, b) != 0) return;
    if (b[510] != 0x55 || b[511] != 0xAA) {
        printf("  Particao em %llu MB: sistema de arquivos nao reconhecido (copia setor a setor)\n",
               (unsigned long long)(off >> 20));
        return;
    }
    if (memcmp(b + 3, "NTFS    ", 8) == 0) map_ntfs(d, off, len, b);
    else if (memcmp(b + 82, "FAT32   ", 8) == 0) map_fat32(d, off, len, b);
    else if (memcmp(b + 3, "EXFAT   ", 8) == 0) map_exfat(d, off, len, b);
    else
        printf("  Particao em %llu MB: sistema de arquivos nao reconhecido (copia setor a setor)\n",
               (unsigned long long)(off >> 20));
}

static int is_ext(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static void scan_partitions(Dev *d)
{
    u8 mbr[512];
    u64 ss = d->sector;
    int i, gpt = 0;
    if (dev_read_any(d, 0, 512, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        printf("  Sem tabela de particoes reconhecida: copia setor a setor\n");
        return;
    }
    for (i = 0; i < 4; i++)
        if (mbr[446 + 16 * i + 4] == 0xEE) gpt = 1;
    if (gpt) {
        u8 *h = xalloc(ss);
        printf("  Tabela de particoes: GPT\n");
        if (dev_read_any(d, ss, ss, h) == 0 && memcmp(h, "EFI PART", 8) == 0) {
            u64 elba = le64(h + 72);
            u32 n = le32(h + 80), esz = le32(h + 84), k;
            if (n <= 1024 && esz >= 128 && esz <= 1024) {
                u8 *e = malloc((size_t)n * esz);
                if (e && dev_read_any(d, elba * ss, (u64)n * esz, e) == 0) {
                    for (k = 0; k < n; k++) {
                        u8 *p = e + (size_t)k * esz;
                        static const u8 zero[16];
                        u64 first = le64(p + 32), last = le64(p + 40);
                        if (memcmp(p, zero, 16) == 0 || last < first) continue;
                        probe_fs(d, first * ss, (last - first + 1) * ss);
                    }
                }
                free(e);
            }
        }
        xfree(h);
        return;
    }
    printf("  Tabela de particoes: MBR\n");
    for (i = 0; i < 4; i++) {
        u8 *e = mbr + 446 + 16 * i;
        u8 t = e[4];
        u64 lba = le32(e + 8), cnt = le32(e + 12);
        if (!t || !cnt) continue;
        if (is_ext(t)) {
            u64 cur = lba;
            int guard = 0;
            while (guard++ < 256) {
                u8 ebr[512];
                u8 *l0 = ebr + 446, *l1 = ebr + 462;
                if (dev_read_any(d, cur * ss, 512, ebr) != 0 || ebr[510] != 0x55 || ebr[511] != 0xAA) break;
                if (l0[4] && le32(l0 + 12))
                    probe_fs(d, (cur + le32(l0 + 8)) * ss, (u64)le32(l0 + 12) * ss);
                if (!is_ext(l1[4]) || !le32(l1 + 8)) break;
                cur = lba + le32(l1 + 8);
            }
        } else {
            probe_fs(d, lba * ss, cnt * ss);
        }
    }
}

/* devolve o fim do segmento homogeneo que comeca em pos; *is_free indica se e livre */
static u64 seg_next(u64 pos, u64 end, int *is_free)
{
    int i;
    u64 nx = end;
    for (i = 0; i < g_nareas; i++) {
        Area *A = &g_areas[i];
        u64 aend = A->start + A->count * A->unit;
        if (pos >= A->start && pos < aend) {
            u64 c = (pos - A->start) / A->unit;
            u64 ce = A->start + (c + 1) * A->unit;
            /* estado: 0 = copiar, 1 = livre, 2 = gravar como zeros */
#define AST(cc) (!bm_get(A->bm, cc) ? 1 : (A->zb && bm_get(A->zb, cc)) ? 2 : 0)
            int st = AST(c);
            while (ce < end && c + 1 < A->count && AST(c + 1) == st) { c++; ce += A->unit; }
#undef AST
            *is_free = st;
            return ce < end ? ce : end;
        }
        if (A->start > pos && A->start < nx) nx = A->start;
    }
    *is_free = 0;
    return nx;
}
