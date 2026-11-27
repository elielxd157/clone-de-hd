/* hdclone - restaurar em disco menor (reducao de particao) */

/* ------------------------------------------------------------------ */
/* restauracao em disco menor: reducao da ultima particao              */
/* ------------------------------------------------------------------ */

typedef struct {
    u64 start, len;
    u8 type;
    int idx;         /* entrada na tabela (MBR/EBR: 0..3; GPT: indice) */
    u64 tbl_off;     /* MBR/EBR: posicao do setor da tabela */
    int logical;     /* particao logica (dentro da estendida) */
    u64 prev_ebr;    /* logica: EBR anterior (cuja entrada 1 aponta para esta), 0 = nenhuma */
} PEnt;

typedef struct {
    int gpt;
    int n;
    PEnt p[128];
    int ext_idx;     /* entrada da particao estendida no MBR, -1 se nao ha */
    u64 ext_start, ext_len;
    u64 gpt_elba;
    u32 gpt_esz;
} PTable;

static int list_parts(Dev *d, PTable *t)
{
    u8 mbr[512];
    u64 ss = d->sector;
    int i;
    memset(t, 0, sizeof(*t));
    t->ext_idx = -1;
    if (dev_read_any(d, 0, 512, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;
    for (i = 0; i < 4; i++)
        if (mbr[446 + 16 * i + 4] == 0xEE) t->gpt = 1;
    if (t->gpt) {
        u8 *h = xalloc(ss), *e;
        u32 n, k;
        if (dev_read_any(d, ss, ss, h) != 0 || memcmp(h, "EFI PART", 8) != 0) { xfree(h); return -1; }
        t->gpt_elba = le64(h + 72);
        n = le32(h + 80);
        t->gpt_esz = le32(h + 84);
        xfree(h);
        if (n > 1024 || t->gpt_esz < 128 || t->gpt_esz > 1024) return -1;
        e = malloc((size_t)n * t->gpt_esz);
        if (!e || dev_read_any(d, t->gpt_elba * ss, (u64)n * t->gpt_esz, e) != 0) { free(e); return -1; }
        for (k = 0; k < n && t->n < 128; k++) {
            u8 *p = e + (size_t)k * t->gpt_esz;
            static const u8 zero[16];
            u64 first = le64(p + 32), last = le64(p + 40);
            if (memcmp(p, zero, 16) == 0 || last < first) continue;
            t->p[t->n].start = first * ss;
            t->p[t->n].len = (last - first + 1) * ss;
            t->p[t->n].idx = (int)k;
            t->n++;
        }
        free(e);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        u8 *e = mbr + 446 + 16 * i;
        u8 ty = e[4];
        u64 lba = le32(e + 8), cnt = le32(e + 12);
        if (!ty || !cnt) continue;
        if (is_ext(ty)) {
            u64 cur = lba, prev = 0;
            int guard = 0;
            t->ext_idx = i;
            t->ext_start = lba * ss;
            t->ext_len = cnt * ss;
            while (guard++ < 256 && t->n < 128) {
                u8 ebr[512];
                u8 *l0 = ebr + 446, *l1 = ebr + 462;
                if (dev_read_any(d, cur * ss, 512, ebr) != 0 || ebr[510] != 0x55 || ebr[511] != 0xAA) break;
                if (l0[4] && le32(l0 + 12)) {
                    PEnt *pe = &t->p[t->n++];
                    pe->start = (cur + le32(l0 + 8)) * ss;
                    pe->len = (u64)le32(l0 + 12) * ss;
                    pe->type = l0[4];
                    pe->idx = 0;
                    pe->tbl_off = cur * ss;
                    pe->logical = 1;
                    pe->prev_ebr = prev;
                }
                if (!is_ext(l1[4]) || !le32(l1 + 8)) break;
                prev = cur * ss;
                cur = lba + le32(l1 + 8);
            }
        } else if (t->n < 128) {
            PEnt *pe = &t->p[t->n++];
            pe->start = lba * ss;
            pe->len = cnt * ss;
            pe->type = ty;
            pe->idx = i;
            pe->tbl_off = 0;
        }
    }
    return 0;
}

typedef struct {
    int active;      /* destino menor: precisa reduzir */
    int has_part;    /* ha uma particao a reduzir */
    int fs;          /* 1 = NTFS, 2 = FAT32 */
    PEnt p;
    u64 new_len;
    u64 new_end;
    int ext_shrink;
    u64 ext_new_len;
    PTable t;
} Plan;

static u64 round_up(u64 v, u64 a) { return (v + a - 1) / a * a; }

#define MIB (1024ull * 1024ull)

/* tamanho minimo que a particao pode ter (bytes); 0 = nao pode ser reduzida
   (why recebe o motivo) */
static u64 fs_min_len(Dev *img, PEnt *pe, int *fs, char *why)
{
    u8 b[512];
    *fs = 0;
    strcpy(why, "sistema de arquivos nao suportado para reducao (so NTFS e FAT32)");
    if (dev_read_any(img, pe->start, 512, b) != 0 || b[510] != 0x55 || b[511] != 0xAA) return 0;
    if (memcmp(b + 3, "NTFS    ", 8) == 0) {
        NtfsGeo g;
        u8 *bm, *rec;
        u64 lu;
        int a;
        Run runs[64];
        int n, i;
        *fs = 1;
        if (ntfs_geo(b, pe->len, &g) != 0 || !(bm = ntfs_load_bitmap(img, pe->start, &g))) {
            strcpy(why, "nao foi possivel ler o NTFS da imagem");
            return 0;
        }
        for (lu = g.ncl; lu > 0 && !bm_get(bm, lu - 1); lu--)
            ;
        free(bm);
        /* $Bitmap precisa ser nao residente; $BadClus nao pode ter clusters defeituosos */
        rec = malloc(g.recsz);
        if (!rec || ntfs_read_rec(img, pe->start, &g, 6, rec) != 0 ||
            (a = ntfs_find_attr(rec, g.recsz, 0x80, NULL)) < 0 || rec[a + 8] == 0) {
            free(rec);
            strcpy(why, "estrutura do NTFS nao suportada para reducao ($Bitmap)");
            return 0;
        }
        if (ntfs_read_rec(img, pe->start, &g, 8, rec) != 0 ||
            (a = ntfs_find_attr(rec, g.recsz, 0x80, "$Bad")) < 0 || rec[a + 8] == 0 ||
            (n = ntfs_runs(rec, (u32)a, runs, 64)) < 0) {
            free(rec);
            strcpy(why, "estrutura do NTFS nao suportada para reducao ($BadClus)");
            return 0;
        }
        for (i = 0; i < n; i++)
            if (runs[i].lcn >= 0) {
                free(rec);
                strcpy(why, "o NTFS registra clusters defeituosos; rode chkdsk e faca o backup de novo");
                return 0;
            }
        free(rec);
        /* clusters usados + setor do boot de reserva no fim */
        return round_up(lu * g.cs + g.bps, g.bps);
    }
    if (memcmp(b + 82, "FAT32   ", 8) == 0) {
        FatGeo g;
        u8 *bm;
        u64 lu, need;
        *fs = 2;
        if (fat_geo(b, pe->len, &g) != 0 || !(bm = fat_load_bitmap(img, pe->start, &g))) {
            strcpy(why, "nao foi possivel ler a FAT32 da imagem");
            return 0;
        }
        for (lu = g.ncl; lu > 0 && !bm_get(bm, lu - 1); lu--)
            ;
        free(bm);
        need = lu > 65525 ? lu : 65525; /* menos que isso deixaria de ser FAT32 */
        return g.data_sec * g.bps + need * g.cs;
    }
    return 0;
}

/* analisa se a imagem cabe num disco de T bytes; morre com explicacao se nao */
static void plan_shrink(Dev *img, u64 T, Plan *pl)
{
    u64 ss = img->sector, reserve, usable, need_disk = 0;
    int i, cross = -1, ncross = 0, bad = 0;
    char msg[700], why[200] = "", s1[32], s2[32];

    memset(pl, 0, sizeof(*pl));
    pl->active = 1;
    if (list_parts(img, &pl->t) != 0)
        die("a imagem nao tem tabela de particoes reconhecida; nao e possivel restaurar em disco menor");
    reserve = pl->t.gpt ? 33 * ss : 0; /* GPT de reserva no fim do disco */
    usable = T - reserve;
    pl->new_end = usable - usable % MIB;

    for (i = 0; i < pl->t.n; i++) {
        PEnt *pe = &pl->t.p[i];
        u64 minlen, req;
        int fs;
        char w[200];
        if (pe->start + pe->len <= usable) continue;
        ncross++;
        minlen = fs_min_len(img, pe, &fs, w);
        if (!minlen) {
            bad = 1;
            req = pe->start + pe->len + reserve;
            snprintf(why, sizeof(why), "particao em %llu MB: %s", (unsigned long long)(pe->start >> 20), w);
        } else {
            req = round_up(pe->start + minlen, MIB) + reserve;
        }
        if (req > need_disk) need_disk = req;
        cross = i;
        pl->fs = fs;
    }
    need_disk = round_up(need_disk, MIB);

    if (ncross == 0) { /* todas as particoes cabem; so a estendida (container) pode precisar ajuste */
        if (pl->t.ext_idx >= 0 && pl->t.ext_start + pl->t.ext_len > usable) {
            u64 maxend = pl->t.ext_start + MIB;
            for (i = 0; i < pl->t.n; i++)
                if (pl->t.p[i].logical && pl->t.p[i].start + pl->t.p[i].len > maxend)
                    maxend = pl->t.p[i].start + pl->t.p[i].len;
            pl->ext_shrink = 1;
            pl->ext_new_len = (maxend > pl->new_end ? maxend : pl->new_end) - pl->t.ext_start;
        }
        printf("\nO disco de destino e menor, mas todas as particoes cabem nele.\n");
        return;
    }
    if (ncross > 1 || bad || need_disk > T || pl->t.p[cross].start >= pl->new_end ||
        pl->t.p[cross].start + MIB > pl->new_end) {
        fmt_size(T, s1);
        fmt_size(need_disk, s2);
        if (bad)
            snprintf(msg, sizeof(msg), "a imagem nao cabe no disco de destino (%s): %s", s1, why);
        else if (ncross > 1)
            snprintf(msg, sizeof(msg),
                     "a imagem nao cabe no disco de destino (%s): mais de uma particao ficaria fora do disco. "
                     "Disco minimo necessario: %s", s1, s2);
        else
            snprintf(msg, sizeof(msg),
                     "os dados nao cabem no disco de destino (%s). Disco minimo necessario: %s. "
                     "Dica: no Windows de origem, use 'Gerenciamento de Disco > Diminuir volume' e "
                     "repita a operacao; isso junta os dados no inicio da particao.", s1, s2);
        die(msg);
    }

    pl->has_part = 1;
    pl->p = pl->t.p[cross];
    pl->new_len = pl->new_end - pl->p.start;
    if (pl->p.logical || (pl->t.ext_idx >= 0 && pl->t.ext_start + pl->t.ext_len > usable)) {
        u64 maxend = pl->new_end;
        for (i = 0; i < pl->t.n; i++)
            if (pl->t.p[i].logical && i != cross && pl->t.p[i].start + pl->t.p[i].len > maxend)
                maxend = pl->t.p[i].start + pl->t.p[i].len;
        pl->ext_shrink = 1;
        pl->ext_new_len = maxend - pl->t.ext_start;
    }
    fmt_size(pl->p.len, s1);
    fmt_size(pl->new_len, s2);
    printf("\nO disco de destino e menor que o original. Plano:\n");
    printf("  Particao em %llu MB (%s) sera reduzida de %s para %s.\n",
           (unsigned long long)(pl->p.start >> 20), pl->fs == 1 ? "NTFS" : "FAT32", s1, s2);
    printf("  Todos os dados usados cabem; nenhum dado precisa ser movido.\n");
}

/* aplica a reducao do NTFS no disco de destino ja restaurado */
static void ntfs_shrink(Dev *d, u64 off, u64 new_len)
{
    u8 *boot = xalloc(4096), *rec = NULL, *orig = NULL, *mir = NULL;
    NtfsGeo g;
    u64 new_tot, ncl_new, bm_size, i;
    int a, nr, k;
    Run runs[64];
    static const u32 recs[3] = {6, 8, 3};

    if (dev_read_any(d, off, 512, boot) != 0 || ntfs_geo(boot, ~0ull, &g) != 0) die("falha ao ler o NTFS restaurado");
    if (g.bps > 512 && dev_read_any(d, off, g.bps, boot) != 0) die("falha ao ler o NTFS restaurado");
    new_tot = new_len / g.bps - 1; /* ultimo setor fica para a copia do boot */
    ncl_new = new_tot * g.bps / g.cs;
    bm_size = round_up((ncl_new + 7) / 8, 8);
    rec = xalloc(g.recsz);
    orig = xalloc(g.recsz);
    mir = xalloc(g.recsz);

    for (k = 0; k < 3; k++) {
        u32 n = recs[k];
        if (ntfs_read_rec(d, off, &g, n, rec) != 0) die("falha ao ler a MFT restaurada");
        memcpy(orig, rec, g.recsz);
        if (n == 6) { /* $Bitmap: novo tamanho; bits alem do fim do volume = 1 */
            Run br[1024];
            int nb;
            u64 pos = 0, bit;
            a = ntfs_find_attr(rec, g.recsz, 0x80, NULL);
            if (a < 0 || rec[a + 8] == 0) die("$Bitmap inesperado");
            if (bm_size > le64(rec + a + 0x28)) die("$Bitmap inesperado (tamanho)");
            put64(rec + a + 0x30, bm_size);
            if (le64(rec + a + 0x38) > bm_size) put64(rec + a + 0x38, bm_size);
            nb = ntfs_runs(rec, (u32)a, br, 1024);
            if (nb < 0) die("$Bitmap inesperado (runlist)");
            /* marca os bits [ncl_new, bm_size*8) como usados, como o formatador faz */
            for (i = 0; i < (u64)nb; i++) {
                u64 rbytes = br[i].len * g.cs;
                u64 lo = ncl_new / 8, hi = bm_size; /* bytes afetados */
                u64 s0 = pos > lo ? pos : lo, e0 = pos + rbytes < hi ? pos + rbytes : hi;
                if (br[i].lcn >= 0 && s0 < e0) {
                    u64 len = e0 - s0;
                    u8 *tmp = malloc((size_t)len);
                    if (!tmp || dev_read_any(d, off + (u64)br[i].lcn * g.cs + (s0 - pos), len, tmp) != 0)
                        die("falha ao ler o $Bitmap restaurado");
                    for (bit = s0 * 8; bit < e0 * 8; bit++)
                        if (bit >= ncl_new) tmp[bit / 8 - s0] |= (u8)(1 << (bit & 7));
                    if (dev_write_any(d, off + (u64)br[i].lcn * g.cs + (s0 - pos), len, tmp) != 0)
                        die("falha ao gravar o $Bitmap");
                    free(tmp);
                }
                pos += rbytes;
            }
        } else if (n == 8) { /* $BadClus:$Bad = uma faixa esparsa do tamanho do volume */
            u32 alen, ro, p, L;
            a = ntfs_find_attr(rec, g.recsz, 0x80, "$Bad");
            if (a < 0 || rec[a + 8] == 0) die("$BadClus inesperado");
            nr = ntfs_runs(rec, (u32)a, runs, 64);
            if (nr < 0) die("$BadClus inesperado (runlist)");
            alen = le32(rec + a + 4);
            ro = le16(rec + a + 0x20);
            for (L = 1; L < 8 && (ncl_new >> (8 * L)) != 0; L++)
                ;
            if (ncl_new >> (8 * L - 1) & 1) L++; /* evita bit de sinal */
            if (ro + 1 + L + 1 > alen) die("$BadClus sem espaco para a runlist");
            p = a + ro;
            memset(rec + p, 0, alen - ro);
            rec[p] = (u8)L; /* tamanho do offset 0 = esparso */
            for (i = 0; i < L; i++) rec[p + 1 + i] = (u8)(ncl_new >> (8 * i));
            put64(rec + a + 0x18, ncl_new - 1);       /* ultimo VCN */
            put64(rec + a + 0x28, ncl_new * g.cs);    /* tamanho alocado */
            put64(rec + a + 0x30, ncl_new * g.cs);    /* tamanho dos dados */
            if (le64(rec + a + 0x38) > ncl_new * g.cs) put64(rec + a + 0x38, ncl_new * g.cs);
        } else { /* $Volume: marca "sujo" para o Windows rodar chkdsk no proximo boot */
            u32 vo;
            a = ntfs_find_attr(rec, g.recsz, 0x70, NULL);
            if (a < 0 || rec[a + 8] != 0) die("$Volume inesperado");
            vo = a + le16(rec + a + 0x14);
            rec[vo + 0x0A] |= 0x01;
        }
        ntfs_protect(rec, g.recsz);
        ntfs_protect(orig, g.recsz);
        if (dev_write_any(d, off + g.mft_lcn * g.cs + (u64)n * g.recsz, g.recsz, rec) != 0)
            die("falha ao gravar a MFT");
        /* atualiza o espelho ($MFTMirr) se ele contem este registro */
        if (dev_read_any(d, off + g.mirr_lcn * g.cs + (u64)n * g.recsz, g.recsz, mir) == 0 &&
            memcmp(mir, orig, g.recsz) == 0 &&
            dev_write_any(d, off + g.mirr_lcn * g.cs + (u64)n * g.recsz, g.recsz, rec) != 0)
            die("falha ao gravar o $MFTMirr");
    }
    /* setor de boot e sua copia no ultimo setor da particao */
    put64(boot + 0x28, new_tot);
    if (dev_write_any(d, off, g.bps, boot) != 0 || dev_write_any(d, off + new_tot * g.bps, g.bps, boot) != 0)
        die("falha ao gravar o setor de boot do NTFS");
    xfree(boot);
    xfree(rec);
    xfree(orig);
    xfree(mir);
    printf("  NTFS reduzido (o Windows vai rodar o chkdsk uma vez no primeiro boot).\n");
}

static void fat32_shrink(Dev *d, u64 off, u64 new_len)
{
    u8 *b = xalloc(4096), *fi = xalloc(4096);
    FatGeo g;
    u32 bk, fsi;
    if (dev_read_any(d, off, 512, b) != 0 || fat_geo(b, ~0ull, &g) != 0) die("falha ao ler a FAT32 restaurada");
    if (dev_read_any(d, off, g.bps, b) != 0) die("falha ao ler a FAT32 restaurada");
    put32(b + 32, (u32)(new_len / g.bps));
    b[19] = b[20] = 0;
    bk = le16(b + 50);
    fsi = le16(b + 48);
    if (dev_write_any(d, off, g.bps, b) != 0) die("falha ao gravar o boot da FAT32");
    if (bk && bk < g.rsv && dev_write_any(d, off + (u64)bk * g.bps, g.bps, b) != 0)
        die("falha ao gravar a copia do boot da FAT32");
    if (fsi && fsi < g.rsv && dev_read_any(d, off + (u64)fsi * g.bps, g.bps, fi) == 0 &&
        le32(fi) == 0x41615252) {
        put32(fi + 488, 0xFFFFFFFFu); /* espaco livre: "desconhecido" (o Windows recalcula) */
        dev_write_any(d, off + (u64)fsi * g.bps, g.bps, fi);
    }
    xfree(b);
    xfree(fi);
    printf("  FAT32 reduzida.\n");
}

/* altera sistema de arquivos e tabela de particoes no destino ja restaurado */
static void apply_shrink(Dev *d, Plan *pl)
{
    u64 ss = d->sector;
    u8 sec[512];
    if (pl->has_part) {
        if (pl->fs == 1) ntfs_shrink(d, pl->p.start, pl->new_len);
        else fat32_shrink(d, pl->p.start, pl->new_len);
    }

    if (!pl->has_part) {
        /* nada a fazer na particao */
    } else if (pl->t.gpt) {
        u64 at = pl->t.gpt_elba * ss + (u64)pl->p.idx * pl->t.gpt_esz;
        u8 e[128];
        if (dev_read_any(d, at, 128, e) != 0) die("falha ao ler a GPT");
        put64(e + 40, (pl->p.start + pl->new_len) / ss - 1);
        if (dev_write_any(d, at, 128, e) != 0) die("falha ao gravar a GPT");
        /* CRCs e copia de reserva: fix_gpt */
    } else {
        u8 *e;
        if (dev_read_any(d, pl->p.tbl_off, 512, sec) != 0) die("falha ao ler a tabela de particoes");
        e = sec + 446 + 16 * pl->p.idx;
        put32(e + 12, (u32)(pl->new_len / ss));
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF; /* CHS final: "use LBA" */
        if (dev_write_any(d, pl->p.tbl_off, 512, sec) != 0) die("falha ao gravar a tabela de particoes");
    }
    if (pl->has_part && !pl->t.gpt) {
        u8 *e;
        if (pl->p.logical && pl->p.prev_ebr) { /* EBR anterior descreve a extensao desta logica */
            if (dev_read_any(d, pl->p.prev_ebr, 512, sec) != 0) die("falha ao ler EBR");
            e = sec + 462;
            put32(e + 12, (u32)((pl->p.start + pl->new_len - pl->p.tbl_off) / ss));
            e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
            if (dev_write_any(d, pl->p.prev_ebr, 512, sec) != 0) die("falha ao gravar EBR");
        }
    }
    if (pl->ext_shrink && !pl->t.gpt) {
        u8 *e;
        {
            if (dev_read_any(d, 0, 512, sec) != 0) die("falha ao ler o MBR");
            e = sec + 446 + 16 * pl->t.ext_idx;
            put32(e + 12, (u32)(pl->ext_new_len / ss));
            e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
            if (dev_write_any(d, 0, 512, sec) != 0) die("falha ao gravar o MBR");
        }
    }
    printf("  Tabela de particoes ajustada.\n");
}
