/* hdclone - recuperar arquivos: FAT32, busca, extracao */

/* ------------------------------------------------------------------ */
/* explorar a imagem: listar, buscar e extrair arquivos (NTFS e FAT32)*/
/* ------------------------------------------------------------------ */

#ifdef _WIN32
typedef wchar_t xchar;
#define XSEP L'\\'
#else
typedef char xchar;
#define XSEP '/'
#endif

typedef struct {
    u64 start, len;
    char label[96];
    char fs[8];
} VolInfo;

static Dev g_vdev; /* disco virtual sobre g_is */

static void open_virtual(const char *path)
{
    imgsrc_open(path);
    memset(&g_vdev, 0, sizeof(g_vdev));
    g_vdev.src = &g_is;
    g_vdev.size = g_is.h.size;
    g_vdev.sector = g_is.h.sector;
    g_vdev.disknum = -1;
    snprintf(g_vdev.name, sizeof(g_vdev.name), "%s", path);
}

/* UTF-16 -> texto para exibir (Windows: pagina de codigo do console; Linux: UTF-8) */
static void u16_to_out(const u16 *w, int n, char *out, int size)
{
#ifdef _WIN32
    int r = WideCharToMultiByte(CP_OEMCP, 0, (const WCHAR *)w, n, out, size - 1, NULL, NULL);
    out[r > 0 ? r : 0] = 0;
#else
    int i, o = 0;
    for (i = 0; i < n && o < size - 5; i++) {
        u32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) {
            out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63));
            out[o++] = (char)(0x80 | (c & 63));
        } else {
            out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63));
            out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63));
        }
    }
    out[o] = 0;
#endif
}

/* texto digitado (Windows: ANSI; Linux: UTF-8) -> UTF-16 */
static int in_to_u16(const char *s, u16 *w, int max)
{
#ifdef _WIN32
    int r = MultiByteToWideChar(CP_ACP, 0, s, -1, (WCHAR *)w, max);
    return r > 0 ? r - 1 : 0;
#else
    int n = 0;
    const u8 *p = (const u8 *)s;
    while (*p && n < max - 2) {
        u32 c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        if (c >= 0x10000) { c -= 0x10000; w[n++] = (u16)(0xD800 + (c >> 10)); w[n++] = (u16)(0xDC00 + (c & 1023)); }
        else w[n++] = (u16)c;
    }
    return n;
#endif
}

/* ---- leitor FAT32 somente leitura ---- */

typedef struct {
    Dev *d;
    u64 off;
    FatGeo g;
    u8 *fc;        /* cache de 64 KB da FAT */
    u64 fc_base;
    u32 fc_len;
} FatVol;

static int fat_vopen(FatVol *f, Dev *d, u64 off, u64 plen)
{
    u8 b[512];
    memset(f, 0, sizeof(*f));
    f->d = d;
    f->off = off;
    if (dev_read_any(d, off, 512, b) != 0 || b[510] != 0x55 || b[511] != 0xAA || memcmp(b + 82, "FAT32   ", 8) != 0 ||
        fat_geo(b, plen, &f->g) != 0)
        return -1;
    f->fc = malloc(65536);
    f->fc_base = ~0ull;
    return f->fc ? 0 : -1;
}

static void fat_vclose(FatVol *f)
{
    free(f->fc);
    f->fc = NULL;
}

static u32 fat_next(FatVol *f, u32 cl)
{
    u64 pos = (u64)cl * 4, base = pos & ~65535ull, fatbytes = (u64)f->g.fatsz * f->g.bps;
    if (cl < 2 || cl >= f->g.ncl + 2) return 0x0FFFFFFF;
    if (f->fc_base != base) {
        u32 n = fatbytes - base < 65536 ? (u32)(fatbytes - base) : 65536;
        if (dev_read_any(f->d, f->off + (u64)f->g.rsv * f->g.bps + base, n, f->fc) != 0) return 0x0FFFFFFF;
        f->fc_base = base;
        f->fc_len = n;
    }
    if (pos - base + 4 > f->fc_len) return 0x0FFFFFFF;
    return le32(f->fc + (pos - base)) & 0x0FFFFFFF;
}

static u64 fat_cl_off(FatVol *f, u32 cl) { return f->off + (f->g.data_sec + (u64)(cl - 2) * f->g.spc) * f->g.bps; }

/* data/hora da FAT (hora local) -> FILETIME */
static u64 fat_time(u16 d, u16 t)
{
    long long y = 1980 + (d >> 9), m = (d >> 5) & 15, dd = d & 31, era, yoe, doy, doe, days, unixt;
    u64 ft;
    if (!d || m < 1 || m > 12 || dd < 1) return 0;
    y -= m <= 2;
    era = y / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + dd - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    unixt = days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2;
    ft = (u64)(unixt + 11644473600LL) * 10000000ull;
#ifdef _WIN32
    {
        FILETIME lf, uf;
        lf.dwLowDateTime = (DWORD)ft;
        lf.dwHighDateTime = (DWORD)(ft >> 32);
        if (LocalFileTimeToFileTime(&lf, &uf)) ft = ((u64)uf.dwHighDateTime << 32) | uf.dwLowDateTime;
    }
#endif
    return ft;
}

/* ---- mapa de um arquivo no disco (NTFS ou FAT32) ---- */

typedef struct {
    u64 off, len; /* posicao no disco e tamanho do trecho */
    int sparse;
} Ext;

typedef struct {
    Ext *e;
    int n, cap;
    u64 size, init_size;
    int resident;
    u8 *res;
    u32 res_len;
} FMap;

static void fm_free(FMap *m)
{
    free(m->e);
    free(m->res);
    memset(m, 0, sizeof(*m));
}

static int fm_add(FMap *m, u64 off, u64 len, int sparse)
{
    if (m->n && m->e[m->n - 1].sparse == sparse && (sparse || m->e[m->n - 1].off + m->e[m->n - 1].len == off)) {
        m->e[m->n - 1].len += len;
        return 0;
    }
    if (m->n == m->cap) {
        int nc = m->cap ? m->cap * 2 : 16;
        Ext *ne = realloc(m->e, sizeof(Ext) * nc);
        if (!ne) return -1;
        m->e = ne;
        m->cap = nc;
    }
    m->e[m->n].off = off;
    m->e[m->n].len = len;
    m->e[m->n].sparse = sparse;
    m->n++;
    return 0;
}

static u64 fm_total(const FMap *m)
{
    u64 t = 0;
    int i;
    for (i = 0; i < m->n; i++) t += m->e[i].len;
    return t;
}

/* le bytes do arquivo (alem dos trechos ou esparso = zeros) */
static int fm_read(Dev *d, const FMap *m, u64 off, u64 len, u8 *out)
{
    u64 pos = 0;
    int i;
    if (m->resident) {
        u64 n = off < m->res_len ? (m->res_len - off < len ? m->res_len - off : len) : 0;
        if (n) memcpy(out, m->res + off, (size_t)n);
        if (n < len) memset(out + n, 0, (size_t)(len - n));
        return 0;
    }
    for (i = 0; i < m->n && len; i++) {
        const Ext *e = &m->e[i];
        if (off < pos + e->len) {
            u64 in = off - pos, n = e->len - in < len ? e->len - in : len;
            if (e->sparse) memset(out, 0, (size_t)n);
            else if (dev_read_any(d, e->off + in, n, out) != 0) return -1;
            out += n; off += n; len -= n;
        }
        pos += e->len;
    }
    if (len) memset(out, 0, (size_t)len);
    return 0;
}

/* posicao no disco do byte "off" do arquivo; -1 se esparso/fora */
static int fm_locate(const FMap *m, u64 off, u64 *disk)
{
    u64 pos = 0;
    int i;
    if (m->resident) return -1;
    for (i = 0; i < m->n; i++) {
        if (off < pos + m->e[i].len) {
            if (m->e[i].sparse) return -1;
            *disk = m->e[i].off + (off - pos);
            return 0;
        }
        pos += m->e[i].len;
    }
    return -1;
}

static int fat_fmap(FatVol *f, u32 first, u64 size, int is_dir, FMap *m)
{
    u32 cl = first;
    u64 guard = 0;
    memset(m, 0, sizeof(*m));
    m->size = m->init_size = size;
    if (first < 2) return size ? -1 : 0;
    while (cl >= 2 && cl < 0x0FFFFFF8 && guard++ <= f->g.ncl) {
        if (cl >= f->g.ncl + 2 || fm_add(m, fat_cl_off(f, cl), f->g.cs, 0) != 0) { fm_free(m); return -1; }
        if (!is_dir && fm_total(m) >= size) break;
        cl = fat_next(f, cl);
    }
    if (is_dir) m->size = m->init_size = fm_total(m);
    else if (fm_total(m) < size) { fm_free(m); return -1; }
    return 0;
}

static u8 lfn_sum(const u8 *e)
{
    u8 sum = 0;
    int i;
    for (i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + e[i]);
    return sum;
}

/* lista uma pasta FAT32 (nomes longos inclusive) */
static int fat_list(FatVol *f, u32 dircl, DirList *dl)
{
    FMap m;
    u8 *buf;
    u64 i;
    u16 lfn[261];
    int lfn_ok = 0;
    u8 sum = 0;
    memset(dl, 0, sizeof(*dl));
    if (fat_fmap(f, dircl, 0, 1, &m) != 0) return -1;
    if (m.size > 64ull * 1024 * 1024 || !(buf = malloc((size_t)m.size + 1))) { fm_free(&m); return -1; }
    if (fm_read(f->d, &m, 0, m.size, buf) != 0) { free(buf); fm_free(&m); return -1; }
    for (i = 0; i + 32 <= m.size; i += 32) {
        u8 *e = buf + i;
        DirEnt *d;
        int k, n = 0;
        if (e[0] == 0) break;
        if (e[0] == 0xE5) { lfn_ok = 0; continue; }
        if (e[11] == 0x0F) { /* pedaco de nome longo */
            int seq = e[0] & 0x1F, idx = (seq - 1) * 13;
            static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            if (e[0] & 0x40) {
                for (k = 0; k < 261; k++) lfn[k] = 0;
                sum = e[13];
                lfn_ok = 1;
            }
            if (seq < 1 || idx + 13 > 260 || e[13] != sum) { lfn_ok = 0; continue; }
            for (k = 0; k < 13; k++) lfn[idx + k] = le16(e + pos[k]);
            continue;
        }
        if (e[11] & 0x08) { lfn_ok = 0; continue; } /* rotulo do volume */
        if (dl->n == dl->cap) {
            int nc = dl->cap ? dl->cap * 2 : 64;
            DirEnt *ne = realloc(dl->e, sizeof(DirEnt) * nc);
            if (!ne) break;
            dl->e = ne;
            dl->cap = nc;
        }
        d = &dl->e[dl->n];
        memset(d, 0, sizeof(*d));
        if (lfn_ok && lfn_sum(e) == sum) {
            while (n < 255 && lfn[n] && lfn[n] != 0xFFFF) { d->name[n] = lfn[n]; n++; }
        }
        if (!n) { /* nome curto 8.3 (com minusculas do NT) */
            int lb = e[12] & 0x08, le = e[12] & 0x10, j;
            for (j = 0; j < 8 && e[j] != ' '; j++) d->name[n++] = (u16)((lb && e[j] >= 'A' && e[j] <= 'Z') ? e[j] + 32 : (j == 0 && e[0] == 0x05 ? 0xE5 : e[j]));
            if (e[8] != ' ') {
                d->name[n++] = '.';
                for (j = 8; j < 11 && e[j] != ' '; j++) d->name[n++] = (u16)((le && e[j] >= 'A' && e[j] <= 'Z') ? e[j] + 32 : e[j]);
            }
        }
        lfn_ok = 0;
        d->nlen = n;
        if ((n == 1 && d->name[0] == '.') || (n == 2 && d->name[0] == '.' && d->name[1] == '.')) continue;
        d->rec = ((u32)le16(e + 20) << 16) | le16(e + 26);
        d->is_dir = (e[11] & 0x10) != 0;
        d->size = d->is_dir ? 0 : le32(e + 28);
        d->mtime = fat_time(le16(e + 24), le16(e + 22));
        dl->n++;
    }
    free(buf);
    fm_free(&m);
    qsort(dl->e, dl->n, sizeof(DirEnt), dirent_order);
    return 0;
}

/* ---- sistema de arquivos generico (NTFS ou FAT32) ---- */

#define FS_NTFS 1
#define FS_FAT32 2

typedef struct {
    int type;
    Ntfs nt;
    FatVol fv;
} Fs;

static int fs_open(Fs *fs, Dev *d, u64 off, u64 len)
{
    memset(fs, 0, sizeof(*fs));
    if (nt_open(&fs->nt, d, off, len) == 0) { fs->type = FS_NTFS; return 0; }
    if (fat_vopen(&fs->fv, d, off, len) == 0) { fs->type = FS_FAT32; return 0; }
    return -1;
}

static void fs_close(Fs *fs)
{
    if (fs->type == FS_NTFS) nt_close(&fs->nt);
    else if (fs->type == FS_FAT32) fat_vclose(&fs->fv);
    fs->type = 0;
}

static Dev *fs_dev(Fs *fs) { return fs->type == FS_NTFS ? fs->nt.d : fs->fv.d; }
static u64 fs_root(Fs *fs) { return fs->type == FS_NTFS ? 5 : fs->fv.g.root; }

static int fs_list(Fs *fs, u64 dir, DirList *dl)
{
    if (fs->type == FS_NTFS) return nt_list(&fs->nt, dir, dl);
    return fat_list(&fs->fv, (u32)dir, dl);
}

/* mapa dos dados de um arquivo. 0 = ok, 1 = formato nao suportado (why), -1 = erro */
static int fs_fmap(Fs *fs, const DirEnt *e, FMap *m, const char **why)
{
    memset(m, 0, sizeof(*m));
    if (fs->type == FS_FAT32) return fat_fmap(&fs->fv, (u32)e->rec, e->size, 0, m);
    {
        AttrData ad;
        int i;
        u64 cs = fs->nt.g.cs;
        if (nt_get_attr(&fs->nt, e->rec, 0x80, NULL, &ad) != 0) return -1;
        if (ad.flags & 0x0001) { *why = "comprimido pelo NTFS"; ad_free(&ad); return 1; }
        if (ad.flags & 0x4000) { *why = "criptografado com EFS"; ad_free(&ad); return 1; }
        m->size = ad.size;
        m->init_size = ad.init_size;
        if (ad.resident) {
            m->resident = 1;
            m->res = ad.res;
            m->res_len = ad.res_len;
            ad.res = NULL;
        } else {
            for (i = 0; i < ad.rl.n; i++)
                if (fm_add(m, ad.rl.r[i].lcn < 0 ? 0 : fs->nt.off + (u64)ad.rl.r[i].lcn * cs, ad.rl.r[i].len * cs,
                           ad.rl.r[i].lcn < 0) != 0) {
                    ad_free(&ad);
                    fm_free(m);
                    return -1;
                }
        }
        ad_free(&ad);
        return 0;
    }
}

static u64 fs_mtime(Fs *fs, const DirEnt *e)
{
    u64 mt = e->mtime;
    if (fs->type == FS_NTFS) {
        AttrData si;
        if (nt_get_attr(&fs->nt, e->rec, 0x10, NULL, &si) == 0 && si.resident && si.res_len >= 16) mt = le64(si.res + 8);
        ad_free(&si);
    }
    return mt;
}

/* lista as particoes NTFS e FAT32 da imagem */
static int image_volumes(Dev *v, VolInfo *out, int max)
{
    PTable *t = malloc(sizeof(PTable));
    int i, n = 0;
    if (!t) return 0;
    if (list_parts(v, t) != 0) { free(t); return 0; }
    for (i = 0; i < t->n && n < max; i++) {
        Fs fs;
        if (fs_open(&fs, v, t->p[i].start, t->p[i].len) != 0) continue;
        out[n].start = t->p[i].start;
        out[n].len = t->p[i].len;
        out[n].label[0] = 0;
        strcpy(out[n].fs, fs.type == FS_NTFS ? "NTFS" : "FAT32");
        if (fs.type == FS_NTFS) {
            AttrData ad;
            if (nt_get_attr(&fs.nt, 3, 0x60, NULL, &ad) == 0 && ad.resident) {
                u16 w[64];
                int k, nl = (int)(ad.res_len / 2 < 64 ? ad.res_len / 2 : 63);
                for (k = 0; k < nl; k++) w[k] = le16(ad.res + 2 * k);
                u16_to_out(w, nl, out[n].label, sizeof(out[n].label));
            }
            ad_free(&ad);
        } else {
            u8 b[512];
            if (dev_read_any(v, t->p[i].start, 512, b) == 0 && memcmp(b + 71, "NO NAME    ", 11) != 0) {
                int k = 11;
                memcpy(out[n].label, b + 71, 11);
                while (k > 0 && (out[n].label[k - 1] == ' ' || !out[n].label[k - 1])) k--;
                out[n].label[k] = 0;
            }
        }
        fs_close(&fs);
        n++;
    }
    free(t);
    return n;
}

/* resolve um caminho ("/" ou "\\" como separador); devolve a entrada */
static int fs_lookup(Fs *fs, const char *path, DirEnt *out)
{
    char comp[512];
    const char *p = path;
    memset(out, 0, sizeof(*out));
    out->rec = fs_root(fs);
    out->is_dir = 1;
    for (;;) {
        int n = 0, i, found = 0;
        u16 w[512];
        int wl;
        DirList dl;
        while (*p == '/' || *p == '\\') p++;
        if (!*p) return 0;
        while (*p && *p != '/' && *p != '\\' && n < 511) comp[n++] = *p++;
        comp[n] = 0;
        if (!out->is_dir) return -1;
        wl = in_to_u16(comp, w, 512);
        if (fs_list(fs, out->rec, &dl) != 0) return -1;
        for (i = 0; i < dl.n; i++)
            if (name_cmp(dl.e[i].name, dl.e[i].nlen, w, wl) == 0) {
                *out = dl.e[i];
                found = 1;
                break;
            }
        dl_free(&dl);
        if (!found) return -1;
    }
}

static void ft_to_str(u64 ft, char *out)
{
    time_t t;
    struct tm *tm;
    if (ft < 116444736000000000ull) { strcpy(out, "                "); return; }
    t = (time_t)(ft / 10000000ull - 11644473600ull);
    tm = localtime(&t);
    if (tm) strftime(out, 20, "%d/%m/%Y %H:%M", tm);
    else strcpy(out, "?");
}

/* ---- extracao ---- */

typedef struct {
    u64 files, dirs, bytes, total, skipped, errors;
    int cancelled;
} XStats;

static XStats g_xs;

static void xs_note(const char *what, const DirEnt *e)
{
    char nm[600];
    u16_to_out(e->name, e->nlen, nm, sizeof(nm));
    printf("  %s: %s\n", what, nm);
}

/* destino + separador + nome (caracteres invalidos no Windows viram "_") */
static void x_join(xchar *out, int max, const xchar *dir, const DirEnt *e)
{
    int n = 0, i;
#ifdef _WIN32
    n = (int)wcslen(dir);
    if (n > max - 2) n = max - 2;
    memcpy(out, dir, n * sizeof(xchar));
    if (n && out[n - 1] != XSEP && out[n - 1] != L'/') out[n++] = XSEP;
    for (i = 0; i < e->nlen && n < max - 1; i++) {
        wchar_t c = e->name[i];
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
        out[n++] = c;
    }
    out[n] = 0;
#else
    char nm[1100];
    n = (int)strlen(dir);
    if (n > max - 2) n = max - 2;
    memcpy(out, dir, n);
    if (n && out[n - 1] != XSEP) out[n++] = XSEP;
    u16_to_out(e->name, e->nlen, nm, sizeof(nm));
    for (i = 0; nm[i] && n < max - 1; i++) out[n++] = nm[i] == '/' ? '_' : nm[i];
    out[n] = 0;
#endif
}

#ifdef _WIN32
/* caminhos longos: prefixo \\?\ (aceito pelas APIs "W" desde o XP) */
static const wchar_t *x_long(const wchar_t *p, wchar_t *buf, int max)
{
    if (wcslen(p) < 240 || !p[0] || p[1] != L':') return p;
    if ((int)wcslen(p) + 5 > max) return p;
    wcscpy(buf, L"\\\\?\\");
    wcscat(buf, p);
    return buf;
}
#endif

static int x_mkdir(const xchar *p)
{
#ifdef _WIN32
    wchar_t b[1100];
    const wchar_t *q = x_long(p, b, 1100);
    return (CreateDirectoryW(q, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
#else
    return (mkdir(p, 0755) == 0 || errno == EEXIST) ? 0 : -1;
#endif
}

/* copia um arquivo da imagem para o disco */
/* copia um arquivo da imagem para o disco */
static void fs_extract_file(Fs *fs, const DirEnt *e, const xchar *path)
{
    FMap m;
    const char *why = NULL;
    u64 off = 0, mtime;
    u8 *buf = NULL;
    int ok = 1, r;
#ifdef _WIN32
    wchar_t lb[1100];
    HANDLE f;
#else
    FILE *f;
#endif
    r = fs_fmap(fs, e, &m, &why);
    if (r < 0) { xs_note("ERRO ao ler (ignorado)", e); g_xs.errors++; return; }
    if (r > 0) {
        char t[80];
        snprintf(t, sizeof(t), "ignorado (%s)", why);
        xs_note(t, e);
        g_xs.skipped++;
        return;
    }
    mtime = fs_mtime(fs, e);
#ifdef _WIN32
    f = CreateFileW(x_long(path, lb, 1100), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { xs_note("ERRO ao criar (ignorado)", e); g_xs.errors++; fm_free(&m); return; }
#else
    f = fopen(path, "wb");
    if (!f) { xs_note("ERRO ao criar (ignorado)", e); g_xs.errors++; fm_free(&m); return; }
#endif
    buf = malloc(CHUNK_SIZE);
    if (!buf) ok = 0;
    while (ok && off < m.size) {
        u64 n = m.size - off < CHUNK_SIZE ? m.size - off : CHUNK_SIZE;
        if (g_cancel) { g_xs.cancelled = 1; ok = 0; break; }
        if (off >= m.init_size) memset(buf, 0, (size_t)n); /* alem do "inicializado" = zeros */
        else {
            u64 rr = m.init_size - off < n ? m.init_size - off : n;
            if (fm_read(fs_dev(fs), &m, off, rr, buf) != 0) { ok = 0; break; }
            if (rr < n) memset(buf + rr, 0, (size_t)(n - rr));
        }
#ifdef _WIN32
        {
            DWORD w;
            if (!WriteFile(f, buf, (DWORD)n, &w, NULL) || w != n) { ok = 0; break; }
        }
#else
        if (fwrite(buf, 1, (size_t)n, f) != n) { ok = 0; break; }
#endif
        off += n;
        g_xs.bytes += n;
        progress(g_xs.bytes < g_xs.total ? g_xs.bytes : g_xs.total, g_xs.total, 0);
    }
    free(buf);
#ifdef _WIN32
    if (mtime) {
        FILETIME ft;
        ft.dwLowDateTime = (DWORD)mtime;
        ft.dwHighDateTime = (DWORD)(mtime >> 32);
        SetFileTime(f, NULL, NULL, &ft);
    }
    CloseHandle(f);
#else
    fclose(f);
    if (mtime > 116444736000000000ull) {
        struct timeval tv[2];
        tv[0].tv_sec = tv[1].tv_sec = (time_t)(mtime / 10000000ull - 11644473600ull);
        tv[0].tv_usec = tv[1].tv_usec = 0;
        utimes(path, tv);
    }
#endif
    fm_free(&m);
    if (g_xs.cancelled) return;
    if (!ok) { xs_note("ERRO na copia (disco cheio ou imagem danificada)", e); g_xs.errors++; }
    else g_xs.files++;
}

/* soma tamanhos (para a barra de progresso) */
static void fs_du(Fs *fs, const DirEnt *e, int depth)
{
    DirList dl;
    int i;
    if (!e->is_dir) { g_xs.total += e->size; return; }
    if (depth > 64 || (e->attr & FA_REPARSE) || fs_list(fs, e->rec, &dl) != 0) return;
    for (i = 0; i < dl.n && !g_cancel; i++) fs_du(fs, &dl.e[i], depth + 1);
    dl_free(&dl);
}

static void fs_extract_entry(Fs *fs, const DirEnt *e, const xchar *dest, int depth)
{
    xchar path[1100];
    DirList dl;
    int i;
    if (g_cancel) { g_xs.cancelled = 1; return; }
    if (e->attr & FA_REPARSE) { xs_note("ignorado (atalho de pasta/junction)", e); g_xs.skipped++; return; }
    x_join(path, 1100, dest, e);
    if (!e->is_dir) { fs_extract_file(fs, e, path); return; }
    if (depth > 64 || x_mkdir(path) != 0) { xs_note("ERRO ao criar pasta", e); g_xs.errors++; return; }
    g_xs.dirs++;
    if (fs_list(fs, e->rec, &dl) != 0) { xs_note("ERRO ao ler pasta", e); g_xs.errors++; return; }
    for (i = 0; i < dl.n && !g_xs.cancelled; i++) fs_extract_entry(fs, &dl.e[i], path, depth + 1);
    dl_free(&dl);
}

/* extrai uma selecao de itens para a pasta dest */
static void extract_entries(Fs *fs, const DirEnt *sel, int nsel, const xchar *dest)
{
    int i;
    char s1[32];
    memset(&g_xs, 0, sizeof(g_xs));
    printf("Calculando tamanho...\n");
    for (i = 0; i < nsel; i++) fs_du(fs, &sel[i], 0);
    if (!g_xs.total) g_xs.total = 1;
    fmt_size(g_xs.total, s1);
    printf("Extraindo %s...\n", s1);
    g_t0 = g_tlast = now_ms();
    for (i = 0; i < nsel && !g_xs.cancelled; i++) fs_extract_entry(fs, &sel[i], dest, 0);
    progress(g_xs.total, g_xs.total, 1);
    fmt_size(g_xs.bytes, s1);
    printf("\n%s: %llu arquivos, %llu pastas, %s", g_xs.cancelled ? "Cancelado" : "Concluido",
           (unsigned long long)g_xs.files, (unsigned long long)g_xs.dirs, s1);
    if (g_xs.skipped) printf(", %llu ignorados", (unsigned long long)g_xs.skipped);
    if (g_xs.errors) printf(", %llu ERROS", (unsigned long long)g_xs.errors);
    printf("\n");
}

/* ---- busca por nome ---- */

typedef struct {
    DirEnt e;
    u16 *path;  /* caminho completo (\pasta\arquivo) */
    int plen;
} Found;

typedef struct {
    Found *f;
    int n, cap;
    u64 dirs;
    int truncated;
} FoundList;

#define MAX_FOUND 20000

static void found_free(FoundList *fl)
{
    int i;
    for (i = 0; i < fl->n; i++) free(fl->f[i].path);
    free(fl->f);
    memset(fl, 0, sizeof(*fl));
}

/* compara com curingas * e ? (sem diferenciar maiusculas) */
static int wild(const u16 *p, int pn, const u16 *s, int sn)
{
    int pi = 0, si = 0, star = -1, mark = 0;
    while (si < sn) {
        if (pi < pn && (p[pi] == '?' || fold16(p[pi]) == fold16(s[si]))) { pi++; si++; }
        else if (pi < pn && p[pi] == '*') { star = pi++; mark = si; }
        else if (star >= 0) { pi = star + 1; si = ++mark; }
        else return 0;
    }
    while (pi < pn && p[pi] == '*') pi++;
    return pi == pn;
}

static void fs_find(Fs *fs, u64 dir, u16 *path, int plen, const u16 *pat, int pn, FoundList *fl, int depth)
{
    DirList dl;
    int i;
    if (depth > 64 || g_cancel || fl->truncated || fs_list(fs, dir, &dl) != 0) return;
    fl->dirs++;
    for (i = 0; i < dl.n && !g_cancel && !fl->truncated; i++) {
        DirEnt *e = &dl.e[i];
        int nl = plen;
        if (nl + 1 + e->nlen >= 4096) continue;
        path[nl++] = '\\';
        memcpy(path + nl, e->name, e->nlen * sizeof(u16));
        nl += e->nlen;
        if (wild(pat, pn, e->name, e->nlen)) {
            if (fl->n >= MAX_FOUND) { fl->truncated = 1; break; }
            if (fl->n == fl->cap) {
                int nc = fl->cap ? fl->cap * 2 : 64;
                Found *nf = realloc(fl->f, sizeof(Found) * nc);
                if (!nf) break;
                fl->f = nf;
                fl->cap = nc;
            }
            fl->f[fl->n].e = *e;
            fl->f[fl->n].path = malloc(nl * sizeof(u16));
            if (!fl->f[fl->n].path) break;
            memcpy(fl->f[fl->n].path, path, nl * sizeof(u16));
            fl->f[fl->n].plen = nl;
            fl->n++;
        }
        if (e->is_dir && !(e->attr & FA_REPARSE)) fs_find(fs, e->rec, path, nl, pat, pn, fl, depth + 1);
    }
    dl_free(&dl);
}

/* busca a partir da pasta "dir"; sem curingas = "contem o texto" */
static void find_files(Fs *fs, u64 dir, const u16 *text, int tn, FoundList *fl)
{
    u16 pat[300], *path = malloc(4096 * sizeof(u16));
    int pn = 0, i, has = 0;
    memset(fl, 0, sizeof(*fl));
    if (!path) return;
    for (i = 0; i < tn; i++)
        if (text[i] == '*' || text[i] == '?') has = 1;
    if (!has) pat[pn++] = '*';
    for (i = 0; i < tn && pn < 297; i++) pat[pn++] = text[i];
    if (!has) pat[pn++] = '*';
    fs_find(fs, dir, path, 0, pat, pn, fl, 0);
    free(path);
}

static void open_volume_cli(const char *img, const char *vol, VolInfo *v, Fs *fs)
{
    int nv, k;
    VolInfo vv[32];
    printf("Abrindo a imagem...\n");
    open_virtual(img);
    printf("\n");
    nv = image_volumes(&g_vdev, vv, 32);
    k = atoi(vol) - 1;
    if (k < 0 || k >= nv) die("numero de particao invalido (veja 'hdclone ls <imagem>')");
    *v = vv[k];
    if (fs_open(fs, &g_vdev, v->start, v->len) != 0) die("nao foi possivel abrir a particao");
}

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_ls(const char *img, const char *vol, const char *path)
{
    int i;
    char sz[32];
    if (!vol) {
        VolInfo v[32];
        int nv;
        printf("Abrindo a imagem...\n");
        open_virtual(img);
        printf("\n");
        nv = image_volumes(&g_vdev, v, 32);
        printf("Particoes na imagem:\n");
        for (i = 0; i < nv; i++) {
            fmt_size(v[i].len, sz);
            printf("  %d  %-5s  %-10s  rotulo: %-20s (inicio em %llu MB)\n", i + 1, v[i].fs, sz,
                   v[i].label[0] ? v[i].label : "-", (unsigned long long)(v[i].start >> 20));
        }
        if (!nv) printf("  (nenhuma particao NTFS ou FAT32)\n");
        printf("\nUse: hdclone ls <imagem> <numero> [pasta]\n");
    } else {
        Fs fs;
        VolInfo v;
        DirEnt d;
        DirList dl;
        open_volume_cli(img, vol, &v, &fs);
        if (fs_lookup(&fs, path ? path : "", &d) != 0 || !d.is_dir) die("pasta nao encontrada");
        if (fs_list(&fs, d.rec, &dl) != 0) die("nao foi possivel ler a pasta");
        for (i = 0; i < dl.n; i++) {
            char nm[600], dt[24];
            u16_to_out(dl.e[i].name, dl.e[i].nlen, nm, sizeof(nm));
            ft_to_str(dl.e[i].mtime, dt);
            if (dl.e[i].is_dir) printf("  %-16s  %12s  %s\\\n", dt, "<PASTA>", nm);
            else printf("  %-16s  %12llu  %s\n", dt, (unsigned long long)dl.e[i].size, nm);
        }
        printf("  %d itens\n", dl.n);
        dl_free(&dl);
        fs_close(&fs);
    }
    imgsrc_close();
}

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_find(const char *img, const char *vol, const char *text, const char *dir)
{
    Fs fs;
    VolInfo v;
    DirEnt d;
    FoundList fl;
    u16 w[300];
    int wl, i;
    open_volume_cli(img, vol, &v, &fs);
    if (fs_lookup(&fs, dir ? dir : "", &d) != 0 || !d.is_dir) die("pasta nao encontrada");
    wl = in_to_u16(text, w, 300);
    printf("Procurando \"%s\"...\n", text);
    find_files(&fs, d.rec, w, wl, &fl);
    for (i = 0; i < fl.n; i++) {
        char nm[1600], dt[24];
        u16_to_out(fl.f[i].path, fl.f[i].plen, nm, sizeof(nm));
        ft_to_str(fl.f[i].e.mtime, dt);
        if (fl.f[i].e.is_dir) printf("  %-16s  %12s  %s\\\n", dt, "<PASTA>", nm);
        else printf("  %-16s  %12llu  %s\n", dt, (unsigned long long)fl.f[i].e.size, nm);
    }
    printf("  %d encontrado(s) em %llu pastas%s\n", fl.n, (unsigned long long)fl.dirs,
           fl.truncated ? " (lista cortada: refine a busca)" : "");
    found_free(&fl);
    fs_close(&fs);
    imgsrc_close();
}

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_extract(const char *img, const char *vol, const char *path, const char *dest)
{
    Fs fs;
    VolInfo v;
    DirEnt e;
    xchar xdest[1100];
    const char *base;
    open_volume_cli(img, vol, &v, &fs);
    if (fs_lookup(&fs, path, &e) != 0) die("arquivo ou pasta nao encontrado na imagem");
    /* nome = ultimo componente do caminho (raiz: "particao") */
    base = path + strlen(path);
    while (base > path && (base[-1] == '/' || base[-1] == '\\')) base--;
    {
        const char *end = base;
        while (base > path && base[-1] != '/' && base[-1] != '\\') base--;
        if (end == base) e.nlen = in_to_u16("particao", e.name, 256);
        else {
            char tmp[512];
            int l = (int)(end - base) < 511 ? (int)(end - base) : 511;
            memcpy(tmp, base, l);
            tmp[l] = 0;
            e.nlen = in_to_u16(tmp, e.name, 256);
        }
    }
    if (!e.is_dir && fs.type == FS_NTFS) {
        AttrData ad;
        if (nt_get_attr(&fs.nt, e.rec, 0x80, NULL, &ad) == 0) e.size = ad.size;
        ad_free(&ad);
    }
#ifdef _WIN32
    MultiByteToWideChar(CP_ACP, 0, dest, -1, xdest, 1100);
#else
    snprintf(xdest, sizeof(xdest), "%s", dest);
#endif
    if (x_mkdir(xdest) != 0) die("nao foi possivel criar a pasta de destino");
    extract_entries(&fs, &e, 1, xdest);
    fs_close(&fs);
    imgsrc_close();
    if (g_xs.errors) die("alguns arquivos nao puderam ser extraidos (veja acima)");
}
