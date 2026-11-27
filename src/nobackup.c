/* hdclone - lista oficial do Windows de arquivos que programas de backup devem ignorar
 *
 * HKLM\SYSTEM\CurrentControlSet\Control\BackupRestore\FilesNotToBackup: cada valor
 * (REG_MULTI_SZ) tem linhas "pasta\mascara", com "/s" = tambem nas subpastas, e
 * variaveis (%windir%, %TEMP%, $UserProfile$...). Aqui a lista e lida do registro
 * SYSTEM do proprio disco e o tamanho de cada item e somado (somente leitura).
 */

/* ---- valores de texto do registro ---- */

/* dados do valor numero idx da chave; devolve 0 e preenche nome (ASCII), tipo e dados */
static int hv_value(Hive *h, u32 nk, u32 idx, char *name, int nmax, u32 *type, const u8 **data, u32 *len)
{
    u8 *n = hv_at(h, nk, 0x4C), *vl, *vk, *d;
    u32 cnt, vo, nl, sz, i;
    if (!n || n[0] != 'n' || n[1] != 'k') return -1;
    cnt = le32(n + 0x24);
    if (idx >= cnt || cnt > 100000 || !(vl = hv_at(h, le32(n + 0x28), 4 * cnt))) return -1;
    vo = le32(vl + 4 * idx);
    if (!(vk = hv_at(h, vo, 0x14)) || vk[0] != 'v' || vk[1] != 'k') return -1;
    nl = le16(vk + 2);
    if (!(vk = hv_at(h, vo, 0x14 + nl))) return -1;
    for (i = 0; i < nl && (int)i < nmax - 1; i++) name[i] = (le16(vk + 0x10) & 1) ? (char)vk[0x14 + i] : '?';
    name[i] = 0;
    *type = le32(vk + 0x0C);
    sz = le32(vk + 4);
    if (sz & 0x80000000u) { /* ate 4 bytes, dentro do proprio vk */
        *data = vk + 8;
        *len = sz & 0x7FFFFFFFu;
        return 0;
    }
    if (sz > 16344 || !(d = hv_at(h, le32(vk + 8), sz))) return -1; /* valores grandes ("db"): ignorados */
    *data = d;
    *len = sz;
    return 0;
}

static u32 hv_current_set(Hive *h, u32 root)
{
    u64 off;
    u32 v, sel = hv_sub(h, root, "Select"), cs = 0;
    char nm[32];
    if (sel && hv_dwords(h, sel, "Current", &off, &v, 1) == 1 && v >= 1 && v <= 99) {
        snprintf(nm, sizeof(nm), "ControlSet%03u", v);
        cs = hv_sub(h, root, nm);
    }
    return cs ? cs : hv_sub(h, root, "ControlSet001");
}

/* ---- conjunto de arquivos ja contados (sem repetir) ---- */

typedef struct {
    u64 *k;
    u32 cap, n;
} RecSet;

static int rs_add(RecSet *s, u64 rec) /* 1 = novo */
{
    u32 i;
    if (s->n * 2 >= s->cap) {
        RecSet t;
        u32 j;
        t.cap = s->cap ? s->cap * 2 : 1024;
        t.n = 0;
        t.k = calloc(t.cap, sizeof(u64));
        if (!t.k) return 0;
        for (j = 0; j < s->cap; j++)
            if (s->k[j]) rs_add(&t, s->k[j] - 1);
        free(s->k);
        *s = t;
    }
    for (i = (u32)((rec * 0x9E3779B97F4A7C15ull) >> 40) & (s->cap - 1);; i = (i + 1) & (s->cap - 1)) {
        if (!s->k[i]) { s->k[i] = rec + 1; s->n++; return 1; }
        if (s->k[i] == rec + 1) return 0;
    }
}

/* ---- relatorio ---- */

#define NB_MAX 96
typedef struct {
    char name[80];
    u64 bytes, files;
    int already; /* pagefile/hiberfil/swapfile: o hdclone ja deixa de fora */
} NbItem;

typedef struct {
    NbItem it[NB_MAX];
    int n;
    u64 total, files, already;
} NbReport;

typedef struct {
    Fs *fs;
    RecSet seen;
    NbItem *cur;
    NbReport *r;
} NbCtx;

static void nb_count_dir(NbCtx *c, u64 dir, const u16 *mask, int ml, int rec, int depth)
{
    DirList dl;
    int i;
    if (depth > 32 || g_cancel || fs_list(c->fs, dir, &dl) != 0) return;
    for (i = 0; i < dl.n; i++) {
        DirEnt *e = &dl.e[i];
        if (e->is_dir) {
            if (rec && !(e->attr & FA_REPARSE)) nb_count_dir(c, e->rec, mask, ml, rec, depth + 1);
        } else if (wild(mask, ml, e->name, e->nlen) && rs_add(&c->seen, e->rec)) {
            int sys = name_eq_ascii(e, "pagefile.sys") || name_eq_ascii(e, "hiberfil.sys") ||
                      name_eq_ascii(e, "swapfile.sys");
            c->cur->bytes += e->size;
            c->cur->files++;
            if (sys) { c->cur->already = 1; c->r->already += e->size; }
            else { c->r->total += e->size; c->r->files++; }
        }
    }
    dl_free(&dl);
}

/* percorre "comp1\comp2\..." a partir de dir (componentes podem ter * e ?) e aplica a mascara */
static void nb_walk(NbCtx *c, u64 dir, const u16 *p, int pl, const u16 *mask, int ml, int rec, int depth)
{
    int s = 0, k;
    DirList dl;
    while (s < pl && p[s] == '\\') s++;
    if (s >= pl) { nb_count_dir(c, dir, mask, ml, rec, 0); return; }
    for (k = s; k < pl && p[k] != '\\'; k++)
        ;
    if (depth > 16 || fs_list(c->fs, dir, &dl) != 0) return;
    for (int i = 0; i < dl.n; i++)
        if (dl.e[i].is_dir && !(dl.e[i].attr & FA_REPARSE) && wild(p + s, k - s, dl.e[i].name, dl.e[i].nlen))
            nb_walk(c, dl.e[i].rec, p + k, pl - k, mask, ml, rec, depth + 1);
    dl_free(&dl);
}

static int u16_starts(const u16 *s, int n, const char *pre)
{
    int i, L = (int)strlen(pre);
    if (n < L) return 0;
    for (i = 0; i < L; i++)
        if (fold16(s[i]) != fold16((u8)pre[i])) return 0;
    return 1;
}

/* uma linha da lista: expande variaveis e conta */
static void nb_pattern(NbCtx *c, const u16 *line, int n)
{
    static const struct { const char *var, *exp; } V[] = {
        {"%systemroot%", "\\Windows"}, {"%windir%", "\\Windows"}, {"%systemdrive%", ""},
        {"%programdata%", "\\ProgramData"}, {"%allusersprofile%", "\\ProgramData"},
        {"$userprofile$", "\\Users\\*"}, {"%userprofile%", "\\Users\\*"},
        {"%localappdata%", "\\Users\\*\\AppData\\Local"}, {"%appdata%", "\\Users\\*\\AppData\\Roaming"},
        {"%temp%", "\\Users\\*\\AppData\\Local\\Temp|\\Windows\\Temp"}, {"%tmp%", "\\Users\\*\\AppData\\Local\\Temp|\\Windows\\Temp"},
        {NULL, NULL}};
    u16 buf[600];
    int rec = 0, i, b = 0, v;
    while (n && (line[n - 1] == ' ' || line[n - 1] == 0)) n--;
    while (n && line[0] == ' ') { line++; n--; }
    if (n >= 2 && line[n - 2] == '/' && fold16(line[n - 1]) == 'S') { rec = 1; n -= 2; while (n && line[n - 1] == ' ') n--; }
    if (n >= 2 && line[1] == ':') { line += 2; n -= 2; } /* "C:\..." */
    for (v = 0; V[v].var; v++)
        if (u16_starts(line, n, V[v].var)) break;
    if (!V[v].var && n && (line[0] == '%' || line[0] == '$')) return; /* variavel desconhecida */
    {
        const char *exp = V[v].var ? V[v].exp : "";
        int skip = V[v].var ? (int)strlen(V[v].var) : 0;
        for (;;) { /* uma ou mais bases separadas por '|' */
            const char *e = strchr(exp, '|');
            int el = e ? (int)(e - exp) : (int)strlen(exp), last, pl;
            b = 0;
            for (i = 0; i < el && b < 590; i++) buf[b++] = (u8)exp[i];
            for (i = skip; i < n && b < 590; i++) buf[b++] = line[i] == '/' ? '\\' : line[i];
            for (last = b; last > 0 && buf[last - 1] != '\\'; last--)
                ;
            pl = last > 0 ? last - 1 : 0;
            if (last < b) nb_walk(c, fs_root(c->fs), buf, pl, buf + last, b - last, rec, 0);
            if (!e) break;
            exp = e + 1;
        }
    }
}

/* le a lista do registro SYSTEM do volume e mede cada item. 0 = ok */
static int nobackup_scan(Fs *fs, NbReport *r, char *why)
{
    DirEnt e;
    FMap m;
    const char *w = NULL;
    Hive h;
    u32 root, cs, key, idx, type, len;
    const u8 *data;
    char vname[80];
    NbCtx c;
    memset(r, 0, sizeof(*r));
    if (fs_lookup(fs, "Windows/System32/config/SYSTEM", &e) != 0 &&
        fs_lookup(fs, "WINNT/system32/config/system", &e) != 0) {
        strcpy(why, "Windows nao encontrado neste volume");
        return -1;
    }
    if (fs_fmap(fs, &e, &m, &w) != 0 || m.size > 512u * 1024 * 1024) { strcpy(why, "registro SYSTEM ilegivel"); fm_free(&m); return -1; }
    h.len = m.size;
    h.b = malloc((size_t)h.len + 8);
    if (!h.b || fm_read(fs_dev(fs), &m, 0, h.len, h.b) != 0 || memcmp(h.b, "regf", 4) != 0) {
        strcpy(why, "registro SYSTEM ilegivel");
        free(h.b);
        fm_free(&m);
        return -1;
    }
    fm_free(&m);
    root = le32(h.b + 0x24);
    cs = hv_current_set(&h, root);
    key = cs ? hv_path(&h, cs, "Control\\BackupRestore\\FilesNotToBackup") : 0;
    if (!key) { strcpy(why, "lista FilesNotToBackup nao encontrada no registro"); free(h.b); return -1; }
    memset(&c, 0, sizeof(c));
    c.fs = fs;
    c.r = r;
    for (idx = 0; hv_value(&h, key, idx, vname, sizeof(vname), &type, &data, &len) == 0 && r->n < NB_MAX; idx++) {
        u32 p = 0;
        if (type != 7 && type != 1 && type != 2) continue; /* REG_MULTI_SZ / REG_SZ / REG_EXPAND_SZ */
        c.cur = &r->it[r->n++];
        snprintf(c.cur->name, sizeof(c.cur->name), "%s", vname);
        while (p + 1 < len) { /* cada linha termina em 0 (UTF-16) */
            u16 line[600];
            int ln = 0;
            while (p + 1 < len && le16(data + p) && ln < 599) { line[ln++] = le16(data + p); p += 2; }
            p += 2;
            if (ln) nb_pattern(&c, line, ln);
        }
    }
    free(c.seen.k);
    free(h.b);
    return 0;
}

static int nb_cmp(const void *a, const void *b)
{
    const NbItem *x = a, *y = b;
    return x->bytes < y->bytes ? 1 : x->bytes > y->bytes ? -1 : 0;
}

/* imprime o relatorio (maiores primeiro) */
static void nobackup_print(NbReport *r, void (*out)(const char *, ...))
{
    int i, shown = 0;
    char s[32];
    qsort(r->it, r->n, sizeof(NbItem), nb_cmp);
    for (i = 0; i < r->n; i++) {
        if (!r->it[i].bytes) continue;
        fmt_size(r->it[i].bytes, s);
        out("             %-36s %10s  %6llu arquivo(s)%s\n", r->it[i].name, s, (unsigned long long)r->it[i].files,
            r->it[i].already ? "  (o hdclone ja deixa de fora)" : "");
        shown++;
    }
    if (!shown) out("             (nenhum arquivo da lista encontrado)\n");
}

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void nb_out_printf(const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("%s", b);
}

/* mede a lista em todas as particoes do disco; devolve quantas tinham Windows */
static int nobackup_disk(Dev *d, void (*out)(const char *, ...), u64 only_start, u64 *total)
{
    PTable *t = malloc(sizeof(PTable));
    int i, found = 0;
    if (total) *total = 0;
    if (!t) return 0;
    if (list_parts(d, t) != 0) { free(t); return 0; }
    for (i = 0; i < t->n; i++) {
        Fs fs;
        NbReport r;
        char why[200], s1[32], s2[32];
        if (only_start && t->p[i].start != only_start) continue;
        if (fs_open(&fs, d, t->p[i].start, t->p[i].len) != 0) continue;
        if (nobackup_scan(&fs, &r, why) == 0) {
            found++;
            out("             particao em %llu MB, lista FilesNotToBackup do Windows:\n",
                (unsigned long long)(t->p[i].start >> 20));
            nobackup_print(&r, out);
            fmt_size(r.total, s1);
            fmt_size(r.already, s2);
            out("             economia adicional possivel: %s em %llu arquivo(s), sem contar pagefile/hiberfil\n"
                "             (%s, que o hdclone ja exclui). Tamanhos antes da compressao.\n",
                s1, (unsigned long long)r.files, s2);
            if (total) *total += r.total;
        } else if (only_start) {
            out("             %s\n", why);
        }
        fs_close(&fs);
    }
    free(t);
    return found;
}

/* hdclone nobackup <disco>: mede, sem alterar nada, quanto a lista economizaria */
#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_nobackup(const char *src)
{
    Dev d;
    int found;
    if (dev_open(&d, src, 0) != 0) die("nao foi possivel abrir o disco (execute como Administrador)");
    printf("\nDisco: %s (somente leitura)\n", d.name);
    found = nobackup_disk(&d, nb_out_printf, 0, NULL);
    dev_close(&d);
    if (!found) die("nenhuma instalacao do Windows com a lista FilesNotToBackup foi encontrada");
}
