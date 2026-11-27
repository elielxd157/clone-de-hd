/* hdclone - autoteste: valida o programa no proprio PC sem gravar em nenhum disco real.
 *
 * Partes que so existem no Windows (somente leitura): lista e le os discos, compara o
 * mapa de espaco usado do C: com o que o Windows informa, detecta BitLocker e cria um
 * snapshot VSS temporario. Depois monta um "disco virtual" num arquivo temporario, com
 * um FAT32 gerado aqui mesmo, e exercita backup, verificacao, restauracao (inclusive em
 * disco menor), clone, formato compativel, retomada e recuperacao de arquivos,
 * conferindo tudo byte a byte. No fim apaga os temporarios e guarda um relatorio.
 */

typedef struct {
    int pass, fail, skip, warn;
} StRes;

static StRes g_st;
static int g_st_bitlocker; /* 1 = C: com BitLocker; 2 = e o VSS entrega os dados abertos */
static char g_st_dir[700], g_st_logpath[700];

/* mostra mesmo com g_quiet (as operacoes internas ficam so no relatorio) */
static void st_say(const char *fmt, ...)
{
    char b[2048];
    int q = g_quiet;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_quiet = 0;
    printf("%s", b);
    g_quiet = q;
}

/* kind: 0 = ok, 1 = falha, 2 = pulado, 3 = atencao (nao e falha do programa) */
static void st_res(int kind, const char *fmt, ...)
{
    static const char *tag[] = {"  [ OK ]   ", "  [FALHA]  ", "  [pulado] ", "  [ATENCAO]"};
    char b[1500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    st_say("%s %s\n", tag[kind], b);
    if (kind == 0) g_st.pass++;
    else if (kind == 1) g_st.fail++;
    else if (kind == 2) g_st.skip++;
    else g_st.warn++;
}

#define ST_CHECK(cond, ...) st_res((cond) ? 0 : 1, __VA_ARGS__)

static void st_run(const char *title, void (*fn)(void))
{
    jmp_buf jb;
    jmp_buf *prev = g_die_jmp;
    u64 t0 = now_ms();
    if (g_cancel) return;
    st_say("\n%s\n", title);
    g_die_jmp = &jb;
    g_quiet = 1;
    if (setjmp(jb) == 0) fn();
    else st_res(1, "erro inesperado: %s", g_die_msg);
    g_quiet = 0;
    g_die_jmp = prev;
    st_say("             (%.1f s)\n", (double)(now_ms() - t0) / 1000.0);
}

static const char *st_path(const char *name)
{
    static char buf[4][800];
    static int k;
    k = (k + 1) & 3;
    snprintf(buf[k], sizeof(buf[k]), "%s%c%s", g_st_dir,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             name);
    return buf[k];
}

/* ---- arquivos auxiliares ---- */

static int st_write_file(const char *path, const u8 *b, u64 n)
{
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) return -1;
    ok = fwrite(b, 1, (size_t)n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

static int st_same_files(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    u8 *x = malloc(CHUNK_SIZE), *y = malloc(CHUNK_SIZE);
    int same = fa && fb && x && y;
    while (same) {
        size_t na = fread(x, 1, CHUNK_SIZE, fa), nb = fread(y, 1, CHUNK_SIZE, fb);
        if (na != nb || memcmp(x, y, na)) same = 0;
        if (!na) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    free(x);
    free(y);
    return same;
}

static void st_rmtree(const char *dir)
{
#ifdef _WIN32
    char pat[800], p[800];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(p, sizeof(p), "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) st_rmtree(p);
            else DeleteFileA(p);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* nomes com acento (extraidos): apaga pela API "W" */
    {
        WCHAR wd[800], wp[1000];
        WIN32_FIND_DATAW fw;
        HANDLE hw;
        MultiByteToWideChar(CP_ACP, 0, dir, -1, wd, 800);
        swprintf(wp, 1000, L"%ls\\*", wd);
        hw = FindFirstFileW(wp, &fw);
        if (hw != INVALID_HANDLE_VALUE) {
            do {
                WCHAR q[1000];
                if (!wcscmp(fw.cFileName, L".") || !wcscmp(fw.cFileName, L"..")) continue;
                swprintf(q, 1000, L"%ls\\%ls", wd, fw.cFileName);
                if (!(fw.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(q);
            } while (FindNextFileW(hw, &fw));
            FindClose(hw);
        }
    }
    RemoveDirectoryA(dir);
#else
    char cmd[900];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) return;
#endif
}

/* ---- disco virtual com FAT32 gerado aqui ---- */

#define ST_DISK (64u * 1024 * 1024)
#define ST_POFF (1024u * 1024)        /* particao comeca em 1 MB */
#define ST_PSEC ((ST_DISK - ST_POFF) / 512)

static u32 g_st_seed;
static u32 st_rnd(void)
{
    g_st_seed ^= g_st_seed << 13;
    g_st_seed ^= g_st_seed >> 17;
    g_st_seed ^= g_st_seed << 5;
    return g_st_seed;
}
static void st_noise(u8 *b, u64 n, u32 seed)
{
    u64 i;
    g_st_seed = seed ? seed : 1;
    for (i = 0; i + 4 <= n; i += 4) put32(b + i, st_rnd());
    for (; i < n; i++) b[i] = (u8)st_rnd();
}

/* conteudo esperado dos arquivos */
static u8 *g_f1, *g_f2, *g_f3;
static u32 g_f1n, g_f2n, g_f3n;
static const u16 g_f1name[] = {'R', 'e', 'l', 'a', 't', 0xF3, 'r', 'i', 'o', ' ', 'd', 'e', ' ', 't', 'e', 's', 't', 'e', ' ', 0xC7, '.', 't', 'x', 't'};
#define F1NAME_LEN 24

static void st_make_contents(void)
{
    u32 i, o = 0;
    if (g_f1) return;
    g_f1n = 400000;
    g_f1 = malloc(g_f1n);
    for (i = 0; o < g_f1n; i++) {
        char l[80];
        int n = snprintf(l, sizeof(l), "linha %u do relatorio de teste do hdclone\r\n", i);
        if (o + n > g_f1n) n = g_f1n - o;
        memcpy(g_f1 + o, l, n);
        o += n;
    }
    g_f2n = 3 * 1024 * 1024 + 777;
    g_f2 = malloc(g_f2n);
    st_noise(g_f2, g_f2n, 7);
    g_f3n = 5000;
    g_f3 = malloc(g_f3n);
    memset(g_f3, 'N', g_f3n);
}

static void st_dirent(u8 *e, const char *n11, u8 attr, u32 cl, u32 size)
{
    memset(e, 0, 32);
    memcpy(e, n11, 11);
    e[11] = attr;
    put32(e + 20, 0);
    e[20] = (u8)(cl >> 16);
    e[21] = (u8)(cl >> 24);
    e[26] = (u8)cl;
    e[27] = (u8)(cl >> 8);
    put32(e + 28, size);
    e[22] = 0; e[23] = 12 << 3;                                   /* 12:00 */
    e[24] = (u8)(((2024 - 1980) << 9 | 5 << 5 | 10) & 0xFF);      /* 10/05/2024 */
    e[25] = (u8)(((2024 - 1980) << 9 | 5 << 5 | 10) >> 8);
}

/* grava o disco virtual: MBR + FAT32 com espaco livre cheio de lixo */
static int st_make_disk(const char *path)
{
    u8 *d = malloc(ST_DISK), *p, *fat1;
    u32 rsv = 32, fatsz = 1, n, data, need, next, c, i, k;
    u32 rootc = 2, f1c, f2a, f2b, dirc, f3c, f2first, f2second;
    if (!d) return -1;
    st_make_contents();
    st_noise(d, ST_DISK, 12345); /* tudo com lixo: o espaco livre deve ficar fora da imagem */
    memset(d, 0, 512);
    /* MBR: 1 particao FAT32 LBA (0x0C) */
    d[446 + 4] = 0x0C;
    put32(d + 446 + 8, ST_POFF / 512);
    put32(d + 446 + 12, ST_PSEC);
    put32(d + 440, 0x12345678);
    d[510] = 0x55; d[511] = 0xAA;
    for (;;) { /* tamanho da FAT */
        n = ST_PSEC - rsv - 2 * fatsz;
        need = ((n + 2) * 4 + 511) / 512;
        if (need <= fatsz) break;
        fatsz = need;
    }
    data = rsv + 2 * fatsz;
    p = d + ST_POFF;
    memset(p, 0, (size_t)rsv * 512);
    memset(p + (size_t)rsv * 512, 0, (size_t)2 * fatsz * 512);
    p[0] = 0xEB; p[1] = 0x58; p[2] = 0x90;
    memcpy(p + 3, "MSWIN4.1", 8);
    p[11] = 0; p[12] = 2;             /* 512 bytes por setor */
    p[13] = 1;                        /* 1 setor por cluster */
    p[14] = (u8)rsv; p[15] = 0;
    p[16] = 2;                        /* 2 FATs */
    p[21] = 0xF8;
    p[24] = 63; p[26] = 255;
    put32(p + 28, ST_POFF / 512);
    put32(p + 32, ST_PSEC);
    put32(p + 36, fatsz);
    put32(p + 44, rootc);
    p[48] = 1; p[50] = 6;             /* FSInfo no setor 1, copia do boot no 6 */
    p[64] = 0x80; p[66] = 0x29;
    put32(p + 67, 0x2024ABCD);
    memcpy(p + 71, "HDCLONTESTE", 11);
    memcpy(p + 82, "FAT32   ", 8);
    p[510] = 0x55; p[511] = 0xAA;
    memcpy(p + 6 * 512, p, 512);
    put32(p + 512, 0x41615252);
    put32(p + 512 + 484, 0x61417272);
    put32(p + 512 + 488, 0xFFFFFFFFu);
    put32(p + 512 + 492, 0xFFFFFFFFu);
    put32(p + 512 + 508, 0xAA550000u);
    fat1 = p + (size_t)rsv * 512;
#define CL(cc) (p + ((size_t)data + (cc) - 2) * 512)
#define SETFAT(cc, v) put32(fat1 + (size_t)(cc) * 4, (v))
    SETFAT(0, 0x0FFFFFF8);
    SETFAT(1, 0x0FFFFFFF);
    SETFAT(rootc, 0x0FFFFFFF);
    next = 3;
    /* arquivo 1 (nome longo com acento), contiguo */
    f1c = next;
    k = (g_f1n + 511) / 512;
    for (i = 0; i < k; i++) {
        SETFAT(next + i, i + 1 < k ? next + i + 1 : 0x0FFFFFFF);
        memset(CL(next + i), 0, 512);
        memcpy(CL(next + i), g_f1 + i * 512, i * 512 + 512 <= g_f1n ? 512 : g_f1n - i * 512);
    }
    next += k;
    /* arquivo 2: fragmentado em 2 pedacos com 300 clusters livres (com lixo) no meio */
    k = (g_f2n + 511) / 512;
    f2first = k / 3;
    f2second = k - f2first;
    f2a = next;
    f2b = next + f2first + 300;
    for (i = 0; i < k; i++) {
        u32 cc = i < f2first ? f2a + i : f2b + (i - f2first);
        u32 nx = i + 1 == k ? 0x0FFFFFFF : (i + 1 < f2first ? cc + 1 : (i + 1 == f2first ? f2b : cc + 1));
        SETFAT(cc, nx);
        memset(CL(cc), 0, 512);
        memcpy(CL(cc), g_f2 + i * 512, i * 512 + 512 <= g_f2n ? 512 : g_f2n - i * 512);
    }
    next = f2b + f2second;
    (void)f2second;
    /* pasta com 1 arquivo */
    dirc = next++;
    f3c = next;
    k = (g_f3n + 511) / 512;
    for (i = 0; i < k; i++) {
        SETFAT(f3c + i, i + 1 < k ? f3c + i + 1 : 0x0FFFFFFF);
        memset(CL(f3c + i), 0, 512);
        memcpy(CL(f3c + i), g_f3 + i * 512, i * 512 + 512 <= g_f3n ? 512 : g_f3n - i * 512);
    }
    next += k;
    SETFAT(dirc, 0x0FFFFFFF);
    memset(CL(dirc), 0, 512);
    st_dirent(CL(dirc), ".          ", 0x10, dirc, 0);
    st_dirent(CL(dirc) + 32, "..         ", 0x10, 0, 0);
    st_dirent(CL(dirc) + 64, "NOTAS   TXT", 0x20, f3c, g_f3n);
    /* pasta raiz: rotulo, nome longo (2 entradas LFN + curta), DADOS.BIN, PASTA, VAZIO.TXT */
    {
        u8 *r = CL(rootc), sn[11];
        u8 sum;
        int seq;
        static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        memset(r, 0, 512);
        st_dirent(r, "HDCLONTESTE", 0x08, 0, 0);
        memcpy(sn, "RELATO~1TXT", 11);
        sum = lfn_sum(sn);
        for (seq = 2; seq >= 1; seq--) {
            u8 *e = r + 32 * (3 - seq);
            memset(e, 0, 32);
            e[0] = (u8)(seq | (seq == 2 ? 0x40 : 0));
            e[11] = 0x0F;
            e[13] = sum;
            for (i = 0; i < 13; i++) {
                u32 ci = (seq - 1) * 13 + i;
                u16 ch = ci < F1NAME_LEN ? g_f1name[ci] : (ci == F1NAME_LEN ? 0 : 0xFFFF);
                e[pos[i]] = (u8)ch;
                e[pos[i] + 1] = (u8)(ch >> 8);
            }
        }
        st_dirent(r + 96, "RELATO~1TXT", 0x20, f1c, g_f1n);
        st_dirent(r + 128, "DADOS   BIN", 0x20, f2a, g_f2n);
        st_dirent(r + 160, "PASTA      ", 0x10, dirc, 0);
        st_dirent(r + 192, "VAZIO   TXT", 0x20, 0, 0);
    }
    memcpy(fat1 + (size_t)fatsz * 512, fat1, (size_t)fatsz * 512); /* segunda FAT */
    (void)c;
#undef CL
#undef SETFAT
    i = st_write_file(path, d, ST_DISK);
    free(d);
    return i;
}

/* confere os arquivos de um disco (arquivo) com FAT32 em ST_POFF */
static int st_check_disk_files(const char *path, char *why)
{
    Dev d;
    Fs fs;
    DirEnt e;
    FMap m;
    const char *w = NULL;
    u8 *buf;
    static const char *names[3] = {NULL, "DADOS.BIN", "PASTA/NOTAS.TXT"};
    const u8 *exp[3];
    u32 expn[3], i;
    int ok = 1;
    exp[0] = g_f1; exp[1] = g_f2; exp[2] = g_f3;
    expn[0] = g_f1n; expn[1] = g_f2n; expn[2] = g_f3n;
    if (dev_open(&d, path, 0) != 0) { strcpy(why, "nao abriu o disco"); return 0; }
#ifndef _WIN32
    { struct stat st; if (!fstat(d.fd, &st)) d.size = (u64)st.st_size; }
#endif
    if (fs_open(&fs, &d, ST_POFF, d.size - ST_POFF) != 0 || fs.type != FS_FAT32) {
        strcpy(why, "FAT32 nao reconhecida");
        dev_close(&d);
        return 0;
    }
    buf = malloc(4 * 1024 * 1024);
    for (i = 0; i < 3 && ok; i++) {
        if (i == 0) { /* nome longo com acento: procura na raiz */
            DirList dl;
            int k, f = 0;
            if (fs_list(&fs, fs_root(&fs), &dl) == 0) {
                for (k = 0; k < dl.n; k++)
                    if (name_cmp(dl.e[k].name, dl.e[k].nlen, g_f1name, F1NAME_LEN) == 0) { e = dl.e[k]; f = 1; }
                dl_free(&dl);
            }
            if (!f) { strcpy(why, "arquivo com nome acentuado nao encontrado"); ok = 0; break; }
        } else if (fs_lookup(&fs, names[i], &e) != 0) {
            sprintf(why, "%s nao encontrado", names[i]);
            ok = 0;
            break;
        }
        if (fs_fmap(&fs, &e, &m, &w) != 0 || m.size != expn[i] || fm_read(&d, &m, 0, m.size, buf) != 0 ||
            memcmp(buf, exp[i], expn[i]) != 0) {
            sprintf(why, "conteudo diferente em %s", i == 0 ? "Relatorio de teste C.txt" : names[i]);
            ok = 0;
        }
        fm_free(&m);
    }
    free(buf);
    fs_close(&fs);
    dev_close(&d);
    return ok;
}

static Opts st_opts(void)
{
    Opts o;
    memset(&o, 0, sizeof(o));
    o.level = 3;
    o.yes = 1;
    o.keep_sys = 1;
    return o;
}

/* ---- etapas ---- */

static void st_env(void)
{
    int cores = ncores();
#ifdef _WIN32
    OSVERSIONINFOA ov;
    memset(&ov, 0, sizeof(ov));
    ov.dwOSVersionInfoSize = sizeof(ov);
    GetVersionExA(&ov);
    {
        DWORD M = ov.dwMajorVersion, m = ov.dwMinorVersion, b = ov.dwBuildNumber;
        const char *nm = M == 5 && m == 0 ? "2000" : M == 5 && m == 1 ? "XP" : M == 5 ? "XP x64 / Server 2003"
                         : M == 6 && m == 0 ? "Vista" : M == 6 && m == 1 ? "7" : M == 6 && m == 2 ? "8"
                         : M == 6 ? "8.1" : M == 10 && b >= 22000 ? "11" : M == 10 ? "10" : "?";
        st_res(0, "Windows %s (%lu.%lu build %lu) %s", nm, M, m, b, ov.szCSDVersion);
    }
    st_res(0, "programa de %d bits%s, %d nucleo(s)", (int)sizeof(void *) * 8,
           is_wow64() ? " (em Windows 64 bits: o VSS nao funciona neste executavel)" : "", cores);
#else
    st_res(0, "Linux, programa de %d bits, %d nucleo(s)", (int)sizeof(void *) * 8, cores);
#endif
    st_res(0, "%s, zstd %s, zlib %s", VERSION_STR, ZSTD_versionString(), zlibVersion());
}

#ifdef _WIN32
static int g_st_sysd = -1;

static void st_disks(void)
{
    int n, cnt = 0;
    Dev d;
    u8 *b;
    char spec[16], sz[32];
    g_st_sysd = system_disk();
    for (n = 0; n < 32; n++) {
        DiskInfo di;
        if (disk_info(n, g_st_sysd, &di) != 0) continue;
        fmt_size(di.size, sz);
        st_say("             disco %d: %s  %s  [%s]%s\n", n, sz, di.model, di.letters,
               di.is_system ? "  <- Windows" : "");
        cnt++;
    }
    if (!cnt) { st_res(1, "nenhum disco acessivel: execute como Administrador"); return; }
    st_res(0, "%d disco(s) encontrado(s)", cnt);
    if (g_st_sysd < 0) { st_res(1, "disco do Windows nao identificado"); return; }
    sprintf(spec, "%d", g_st_sysd);
    if (dev_open(&d, spec, 0) != 0) { st_res(1, "nao abriu o disco do Windows (execute como Administrador)"); return; }
    b = xalloc(CHUNK_SIZE);
    ST_CHECK(dev_pread(&d, b, CHUNK_SIZE, 0) == 0 && b[510] == 0x55 && b[511] == 0xAA,
             "leitura do inicio do disco %d (tabela de particoes valida)", g_st_sysd);
    ST_CHECK(d.size > CHUNK_SIZE && dev_pread(&d, b, CHUNK_SIZE, (d.size - CHUNK_SIZE) / d.sector * d.sector) == 0,
             "leitura do fim do disco (tamanho informado correto, setor de %u bytes)", d.sector);
    xfree(b);
    dev_close(&d);
}

/* offset da particao do C: no disco */
static int st_c_offset(u64 *off, int *disk)
{
    HANDLE h = CreateFileA("\\\\.\\C:", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    u8 buf[2048];
    DWORD br;
    VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)buf;
    int ok;
    if (h == INVALID_HANDLE_VALUE) return -1;
    ok = DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof(buf), &br, NULL) &&
         e->NumberOfDiskExtents == 1;
    if (ok) { *off = (u64)e->Extents[0].StartingOffset.QuadPart; *disk = (int)e->Extents[0].DiskNumber; }
    CloseHandle(h);
    return ok ? 0 : -1;
}

static void st_cmap(void)
{
    u64 off, used = 0, wused;
    int disk, i, found = 0;
    Dev d;
    u8 b[512];
    char spec[16], s1[32], s2[32];
    ULARGE_INTEGER av, tot, fr;
    if (st_c_offset(&off, &disk) != 0) { st_res(2, "nao foi possivel localizar o C: (volume em mais de um disco?)"); return; }
    sprintf(spec, "%d", disk);
    if (dev_open(&d, spec, 0) != 0) { st_res(1, "nao abriu o disco (execute como Administrador)"); return; }
    if (dev_read_any(&d, off, 512, b) != 0) { st_res(1, "nao leu o setor de boot do C:"); dev_close(&d); return; }
    if (!memcmp(b + 3, "-FVE-FS-", 8)) {
        g_st_bitlocker = 1;
        st_res(3, "o C: esta CRIPTOGRAFADO com BitLocker (no disco os dados estao cifrados)");
        st_res(2, "mapa de espaco usado do C: (com BitLocker so da para ler pelo VSS)");
        dev_close(&d);
        return;
    }
    if (memcmp(b + 3, "NTFS    ", 8) != 0) { st_res(2, "o C: nao e NTFS"); dev_close(&d); return; }
    g_keep_sys = 1;
    g_skip_restore = 0;
    reset_areas();
    scan_partitions(&d);
    for (i = 0; i < g_nareas; i++)
        if (g_areas[i].start == off) { used = count_used(g_areas[i].bm, g_areas[i].count) * g_areas[i].unit; found = 1; }
    reset_areas();
    dev_close(&d);
    if (!found) { st_res(1, "nao foi possivel ler o mapa do NTFS do C:"); return; }
    if (!GetDiskFreeSpaceExA("C:\\", &av, &tot, &fr)) { st_res(2, "Windows nao informou o espaco do C:"); return; }
    wused = tot.QuadPart - fr.QuadPart;
    fmt_size(used, s1);
    fmt_size(wused, s2);
    {
        u64 diff = used > wused ? used - wused : wused - used, tol = tot.QuadPart / 100 + 256ull * 1024 * 1024;
        ST_CHECK(diff <= tol, "espaco usado no C: lido pelo hdclone = %s; segundo o Windows = %s", s1, s2);
    }
}

static void st_vss(void)
{
    int i;
    u64 t0 = now_ms();
    if (g_st_sysd < 0) g_st_sysd = system_disk();
    if (g_st_sysd < 0) { st_res(2, "disco do Windows nao identificado"); return; }
    if (is_wow64()) {
        st_res(2, "VSS: este e o programa de 32 bits num Windows de 64 bits; deixe o hdclone64/hdclone-gui64 "
                  "na mesma pasta");
        return;
    }
    if (vss_begin(g_st_sysd) != 0) { st_res(1, "VSS: %s", g_vss_note); return; }
    st_res(0, "VSS: snapshot criado em %.1f s (%d volume(s))", (double)(now_ms() - t0) / 1000.0, g_nsnap);
    for (i = 0; i < g_nsnap; i++) {
        u8 *s = xalloc(4096), *r = xalloc(4096);
        char spec[16];
        Dev d;
        int rawok = 0;
        sprintf(spec, "%d", g_st_sysd);
        if (dev_open(&d, spec, 0) == 0) { rawok = dev_read_any(&d, g_snap[i].start, 512, r) == 0; dev_close(&d); }
        if (h_pread(g_snap[i].h, s, 4096, 0) != 0) {
            st_res(1, "VSS: nao leu o snapshot do volume %s", g_snap[i].label);
        } else if (rawok && !memcmp(r + 3, "-FVE-FS-", 8) && !memcmp(s + 3, "NTFS    ", 8)) {
            g_st_bitlocker = 2;
            st_res(3, "volume %s: com BitLocker, o snapshot entrega os dados DESCRIPTOGRAFADOS. A imagem "
                      "ficaria sem criptografia (risco confirmado neste PC).", g_snap[i].label);
        } else {
            /* sem BitLocker, o 1o setor visto pelo snapshot e igual ao do disco */
            const char *fs = !memcmp(s + 3, "NTFS    ", 8) ? "NTFS" : !memcmp(s + 82, "FAT32   ", 8) ? "FAT32"
                             : !memcmp(s + 3, "EXFAT   ", 8) ? "exFAT" : "outro";
            ST_CHECK(rawok && !memcmp(s, r, 512), "VSS: leitura pelo snapshot do volume %s (%s)", g_snap[i].label, fs);
        }
        xfree(s);
        xfree(r);
    }
    vss_end(0);
    st_res(0, "VSS: snapshot descartado");
}
#endif

static void st_virtual(void)
{
    Opts o;
    char why[200] = "", disk[800], img[800];
    u64 isz, avail = 0;
    int fat = 0, k;
    i64 ps;
    snprintf(disk, sizeof(disk), "%s", st_path("disco.img"));
    snprintf(img, sizeof(img), "%s", st_path("imagem.hdc"));
    if (dest_info(disk, &avail, &fat) == 0 && avail < 700ull * 1024 * 1024) {
        st_res(2, "pouco espaco na pasta temporaria (precisa de ~700 MB)");
        return;
    }
    ST_CHECK(st_make_disk(disk) == 0, "disco virtual de 64 MB com FAT32, nome acentuado e arquivo fragmentado");
    ST_CHECK(st_check_disk_files(disk, why), "leitor de FAT32 confere o disco original%s%s", why[0] ? ": " : "", why);
    /* backup inteligente em partes de 2 MB com verificacao */
    o = st_opts();
    o.split = 2ull * 1024 * 1024;
    o.verify = 1;
    why[0] = 0;
    cmd_backup(disk, img, &o);
    for (isz = 0, k = 1; (ps = part_size(img, k)) >= 0; k++) isz += (u64)ps;
    st_res(0, "backup + verificacao (zstd, %d threads, %s)", ncores(), g_last_direct ? "gravacao direta" : "gravacao pelo cache");
    ST_CHECK(isz < ST_DISK / 4, "so o espaco usado foi copiado: imagem de %.1f MB para disco de 64 MB", (double)isz / 1048576);
    ST_CHECK(k > 3, "imagem dividida em %d partes", k - 1);
    /* restauracao */
    remove(st_path("r1.img"));
    cmd_restore(img, st_path("r1.img"), &o);
    ST_CHECK(st_check_disk_files(st_path("r1.img"), why), "restauracao: arquivos identicos%s%s", why[0] ? " -- " : "", why);
    /* disco menor */
    {
        u8 *z = calloc(1, 48u * 1024 * 1024);
        st_write_file(st_path("r2.img"), z, 48u * 1024 * 1024);
        free(z);
    }
    g_shrunk = 0;
    o.verify = 0;
    cmd_restore(img, st_path("r2.img"), &o);
    ST_CHECK(g_shrunk && st_check_disk_files(st_path("r2.img"), why),
             "restauracao em disco MENOR (48 MB): particao reduzida e arquivos identicos%s%s", why[0] ? " -- " : "", why);
}

static void st_more(void)
{
    Opts o = st_opts();
    char why[200] = "", disk[800], img[800];
    snprintf(disk, sizeof(disk), "%s", st_path("disco.img"));
    snprintf(img, sizeof(img), "%s", st_path("imagem.hdc"));
    if (part_size(img, 1) < 0) { st_res(2, "depende da etapa anterior (imagem nao foi criada)"); return; }
    /* imagem aberta pelo indice + busca + extracao */
    {
        VolInfo v[4];
        Fs fs;
        FoundList fl;
        DirEnt root;
        u16 q[] = {'r', 'e', 'l', 'a', 't', '*'};
        xchar dest[800];
        int nv;
        g_idx_used = 0;
        open_virtual(img);
        nv = image_volumes(&g_vdev, v, 4);
        ST_CHECK(g_idx_used, "imagem aberta pelo indice (sem percorrer a imagem inteira)");
        ST_CHECK(nv == 1 && !strcmp(v[0].fs, "FAT32") && !strcmp(v[0].label, "HDCLONTESTE"),
                 "particao encontrada na imagem: %s \"%s\"", nv ? v[0].fs : "-", nv ? v[0].label : "");
        if (nv == 1 && fs_open(&fs, &g_vdev, v[0].start, v[0].len) == 0) {
            find_files(&fs, fs_root(&fs), q, 6, &fl);
            ST_CHECK(fl.n == 1 && fl.f[0].e.nlen == F1NAME_LEN, "busca por nome com curinga (relat*)");
            found_free(&fl);
            memset(&root, 0, sizeof(root));
            root.rec = fs_root(&fs);
            root.is_dir = 1;
            root.nlen = in_to_u16("particao", root.name, 256);
#ifdef _WIN32
            MultiByteToWideChar(CP_ACP, 0, st_path("extraido"), -1, dest, 800);
#else
            snprintf(dest, sizeof(dest), "%s", st_path("extraido"));
#endif
            x_mkdir(dest);
            extract_entries(&fs, &root, 1, dest);
            ST_CHECK(g_xs.files == 4 && g_xs.errors == 0, "recuperar arquivos: %llu arquivos extraidos",
                     (unsigned long long)g_xs.files);
            {
                char pa[900];
                FILE *f;
                u8 *b = malloc(g_f2n + 1);
                size_t n = 0;
                snprintf(pa, sizeof(pa), "%s%cparticao%cDADOS.BIN", st_path("extraido"),
#ifdef _WIN32
                         '\\', '\\'
#else
                         '/', '/'
#endif
                );
                f = fopen(pa, "rb");
                if (f) { n = fread(b, 1, g_f2n + 1, f); fclose(f); }
                ST_CHECK(n == g_f2n && !memcmp(b, g_f2, g_f2n), "arquivo fragmentado extraido identico");
                free(b);
            }
            fs_close(&fs);
        }
        imgsrc_close();
    }
    /* clone com verificacao */
    remove(st_path("c1.img"));
    o.verify = 1;
    cmd_clone(disk, st_path("c1.img"), &o);
    ST_CHECK(st_check_disk_files(st_path("c1.img"), why), "clone disco -> disco com verificacao%s%s", why[0] ? " -- " : "", why);
    /* setor a setor: identico byte a byte */
    o = st_opts();
    o.raw = 1;
    cmd_backup(disk, st_path("raw.hdc"), &o);
    remove(st_path("r3.img"));
    cmd_restore(st_path("raw.hdc"), st_path("r3.img"), &o);
    ST_CHECK(st_same_files(disk, st_path("r3.img")), "modo setor a setor: restauracao identica byte a byte");
    /* formato compativel (zlib) */
    o = st_opts();
    o.compat = 1;
    o.level = 6;
    cmd_backup(disk, st_path("compat.hdc"), &o);
    remove(st_path("r4.img"));
    cmd_restore(st_path("compat.hdc"), st_path("r4.img"), &o);
    ST_CHECK(st_check_disk_files(st_path("r4.img"), why), "formato compativel com versoes antigas (zlib)");
}

static void st_resume(void)
{
    Opts o = st_opts();
    char disk[800], img[800];
    FILE *f;
    i64 sz;
    jmp_buf jb, *prev = g_die_jmp;
    int died = 0;
    snprintf(disk, sizeof(disk), "%s", st_path("disco.img"));
    snprintf(img, sizeof(img), "%s", st_path("raw.hdc"));
    sz = part_size(img, 1);
    if (sz < 0) { st_res(2, "depende da etapa anterior (imagem setor a setor nao foi criada)"); return; }
    /* simula uma interrupcao: corta a imagem no meio */
#ifdef _WIN32
    {
        HANDLE h = CreateFileA(img, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        LARGE_INTEGER li;
        li.QuadPart = sz / 2;
        if (h != INVALID_HANDLE_VALUE) { SetFilePointerEx(h, li, NULL, FILE_BEGIN); SetEndOfFile(h); CloseHandle(h); }
    }
#else
    if (truncate(img, (off_t)(sz / 2)) != 0) { st_res(1, "nao foi possivel simular a interrupcao"); return; }
#endif
    o.raw = 1;
    o.resume = 1;
    cmd_backup(disk, img, &o);
    remove(st_path("r5.img"));
    o.resume = 0;
    cmd_restore(img, st_path("r5.img"), &o);
    ST_CHECK(st_same_files(disk, st_path("r5.img")), "continuar backup interrompido: resultado identico");
    /* imagem corrompida tem que ser detectada */
    f = fopen(img, "r+b");
    if (f) { fseek(f, (long)(sz / 3), SEEK_SET); fputs("XXXXXXXXXXXXXXXX", f); fclose(f); }
    g_die_jmp = &jb;
    if (setjmp(jb) == 0) cmd_verify(img);
    else died = strstr(g_die_msg, "CRC") || strstr(g_die_msg, "corromp");
    g_die_jmp = prev;
    ST_CHECK(died, "imagem corrompida e detectada na verificacao");
}

static void st_speed(void)
{
    u8 *src = malloc(16 * CHUNK_SIZE), *dst = malloc(CBUF_SIZE);
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    u64 t0, t1, in = 0, out = 0;
    int i;
    if (!src || !dst || !cc) { st_res(2, "sem memoria para medir"); free(src); free(dst); ZSTD_freeCCtx(cc); return; }
    st_make_contents();
    for (i = 0; i < 16; i++) { /* metade texto, metade dados aleatorios */
        if (i & 1) st_noise(src + (u64)i * CHUNK_SIZE, CHUNK_SIZE, 99 + i);
        else memcpy(src + (u64)i * CHUNK_SIZE, g_f1, CHUNK_SIZE < g_f1n ? CHUNK_SIZE : g_f1n);
    }
    t0 = now_ms();
    for (i = 0; i < 16; i++) {
        size_t r = ZSTD_compressCCtx(cc, dst, CBUF_SIZE, src + (u64)i * CHUNK_SIZE, CHUNK_SIZE, 3);
        in += CHUNK_SIZE;
        out += ZSTD_isError(r) ? CHUNK_SIZE : r;
    }
    t1 = now_ms();
    if (t1 == t0) t1 = t0 + 1;
    st_res(0, "compressao zstd nivel 3: %.0f MB/s por nucleo (x %d nucleos no backup)",
           (double)in / 1048576.0 / ((double)(t1 - t0) / 1000.0), ncores());
    ZSTD_freeCCtx(cc);
    free(src);
    free(dst);
}

static void cmd_selftest(void)
{
    char t[64], base[600];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    memset(&g_st, 0, sizeof(g_st));
#ifdef _WIN32
    if (!GetTempPathA(sizeof(base), base)) strcpy(base, ".\\");
    snprintf(g_st_dir, sizeof(g_st_dir), "%shdclone-autoteste-%lu", base, (unsigned long)GetTickCount());
    snprintf(g_st_logpath, sizeof(g_st_logpath), "%shdclone-autoteste.log", base);
    CreateDirectoryA(g_st_dir, NULL);
#else
    snprintf(base, sizeof(base), "%s", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    snprintf(g_st_dir, sizeof(g_st_dir), "%s/hdclone-autoteste-%lu", base, (unsigned long)now);
    snprintf(g_st_logpath, sizeof(g_st_logpath), "%s/hdclone-autoteste.log", base);
    mkdir(g_st_dir, 0700);
#endif
    g_logf = fopen(g_st_logpath, "w");
    g_log_lock = 1;
    if (tm) strftime(t, sizeof(t), "%d/%m/%Y %H:%M:%S", tm); else strcpy(t, "?");
    st_say("AUTOTESTE do hdclone - %s\n(nenhum disco e alterado: os testes de gravacao usam arquivos temporarios)\n", t);
    st_run("1. Ambiente", st_env);
#ifdef _WIN32
    st_run("2. Discos (somente leitura)", st_disks);
    st_run("3. Mapa de espaco usado do C: (somente leitura)", st_cmap);
    st_run("4. VSS: snapshot temporario do disco do Windows (somente leitura)", st_vss);
#endif
    st_run("5. Disco virtual: backup, verificacao e restauracao", st_virtual);
    st_run("6. Disco virtual: recuperar arquivos, clone, setor a setor e formato compativel", st_more);
    st_run("7. Disco virtual: backup interrompido e imagem corrompida", st_resume);
    st_run("8. Desempenho", st_speed);
    st_rmtree(g_st_dir);
    if (g_cancel) st_say("\nAutoteste cancelado.\n");
    st_say("\nRESULTADO: %d ok, %d falha(s), %d pulado(s), %d aviso(s)\n", g_st.pass, g_st.fail, g_st.skip, g_st.warn);
    if (g_st_bitlocker)
        st_say("\nIMPORTANTE: este PC usa BitLocker no disco do Windows. %s\n",
               g_st_bitlocker == 2 ? "Confirmado: o backup com VSS gravaria os dados SEM criptografia."
                                   : "O backup com VSS pode gravar os dados sem criptografia.");
    st_say("Relatorio salvo em: %s\n", g_st_logpath);
    g_log_lock = 0;
    if (g_logf) { fclose(g_logf); g_logf = NULL; }
}
