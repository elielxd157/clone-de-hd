/* hdclone - acesso a discos e arquivos */

/* ------------------------------------------------------------------ */
/* acesso a disco / arquivo                                            */
/* ------------------------------------------------------------------ */

/* VSS: faixas do disco que devem ser lidas de um snapshot em vez do disco */
#ifdef _WIN32
typedef HANDLE fh_t;
#else
typedef int fh_t; /* no Linux: usado so pelos testes (HDCLONE_TEST_SNAP) */
#define MAX_PATH 260
#endif
typedef struct {
    u64 start, len;  /* posicao do volume no disco */
    fh_t h;          /* dispositivo do snapshot (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN) */
    char vol[MAX_PATH]; /* nome do volume (\\?\Volume{...}, sem barra final) */
    char label[64];
} SnapMap;
#define MAX_SNAPS 32
static SnapMap g_snap[MAX_SNAPS];
static int g_nsnap;

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    int use_snap; /* leituras redirecionadas para os snapshots VSS */
    void *src;    /* "disco virtual": leitura vem de dentro de uma imagem .hdc */
    int is_device;
    int disknum; /* Windows: numero do PhysicalDrive, -1 se arquivo */
    u64 size;
    u32 sector;
    char name[600];
} Dev;

static int dev_open(Dev *d, const char *spec, int write)
{
    memset(d, 0, sizeof(*d));
    d->disknum = -1;
    d->sector = 512;
#ifdef _WIN32
    if (is_number(spec)) {
        sprintf(d->name, "\\\\.\\PhysicalDrive%s", spec);
        d->disknum = atoi(spec);
        d->is_device = 1;
    } else {
        strncpy(d->name, spec, sizeof(d->name) - 1);
        d->is_device = strncmp(spec, "\\\\.\\", 4) == 0;
    }
    d->h = CreateFileA(d->name, GENERIC_READ | (write ? GENERIC_WRITE : 0),
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       (write && !d->is_device) ? OPEN_ALWAYS : OPEN_EXISTING,
                       0, NULL);
    if (d->h == INVALID_HANDLE_VALUE) return -1;
    res_add(RES_HANDLE, d->h);
    if (d->is_device) {
        DISK_GEOMETRY_EX gx;
        DISK_GEOMETRY g;
        GET_LENGTH_INFORMATION li;
        DWORD br;
        if (DeviceIoControl(d->h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &gx, sizeof(gx), &br, NULL)) {
            d->sector = gx.Geometry.BytesPerSector;
            d->size = (u64)gx.DiskSize.QuadPart;
        } else if (DeviceIoControl(d->h, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &g, sizeof(g), &br, NULL)) {
            d->sector = g.BytesPerSector;
            d->size = (u64)g.Cylinders.QuadPart * g.TracksPerCylinder * g.SectorsPerTrack * g.BytesPerSector;
        }
        if (DeviceIoControl(d->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof(li), &br, NULL))
            d->size = (u64)li.Length.QuadPart;
        if (d->sector < 512 || d->sector > 65536) d->sector = 512;
    } else {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(d->h, &sz)) d->size = (u64)sz.QuadPart;
    }
#else
    struct stat st;
    strncpy(d->name, spec, sizeof(d->name) - 1);
    d->fd = open(spec, write ? (O_RDWR | O_CREAT) : O_RDONLY, 0644);
    if (d->fd < 0) return -1;
    res_add(RES_HANDLE, (void *)(intptr_t)(d->fd + 1));
    if (fstat(d->fd, &st) == 0) {
        if (S_ISBLK(st.st_mode)) {
            d->is_device = 1;
#ifdef __linux__
            {
                u64 sz = 0;
                int ss = 512;
                ioctl(d->fd, BLKGETSIZE64, &sz);
                ioctl(d->fd, BLKSSZGET, &ss);
                d->size = sz;
                d->sector = (u32)ss;
            }
#endif
        } else {
            d->size = (u64)st.st_size;
        }
    }
#endif
    return 0;
}

static void dev_close(Dev *d)
{
#ifdef _WIN32
    if (d->h && d->h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(d->h);
        res_del(d->h);
        CloseHandle(d->h);
    }
    d->h = NULL;
#else
    if (d->fd > 0) { fsync(d->fd); res_del((void *)(intptr_t)(d->fd + 1)); close(d->fd); }
    d->fd = -1;
#endif
}

static int h_pread(fh_t h, void *buf, u32 len, u64 off)
{
#ifdef _WIN32
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return -1;
    if (!ReadFile(h, buf, len, &got, NULL) || got != len) return -1;
    return 0;
#else
    u8 *p = buf;
    while (len) {
        ssize_t r = pread(h, p, len, (off_t)off);
        if (r <= 0) return -1;
        p += r; off += (u64)r; len -= (u32)r;
    }
    return 0;
#endif
}

#ifndef _WIN32
/* Teste (so no Linux): simula setores defeituosos.
   HDCLONE_TEST_BAD="setor:quantidade,..."  HDCLONE_TEST_BADMS=atraso por falha */
static u64 g_tb_s[64], g_tb_n[64];
static int g_tb_cnt = -1, g_tb_ms;
static volatile u64 g_tb_fails;
static int test_bad(Dev *d, u64 off, u32 len)
{
    int i;
    if (d->src) return 0;
    if (g_tb_cnt < 0) {
        const char *e = getenv("HDCLONE_TEST_BAD");
        g_tb_cnt = 0;
        g_tb_ms = getenv("HDCLONE_TEST_BADMS") ? atoi(getenv("HDCLONE_TEST_BADMS")) : 0;
        while (e && *e && g_tb_cnt < 64) {
            unsigned long long a, b;
            int u = 0;
            if (sscanf(e, "%llu:%llu%n", &a, &b, &u) != 2) break;
            g_tb_s[g_tb_cnt] = a;
            g_tb_n[g_tb_cnt++] = b;
            e += u;
            if (*e == ',') e++;
        }
    }
    for (i = 0; i < g_tb_cnt; i++) {
        u64 bs = g_tb_s[i] * 512, be = bs + g_tb_n[i] * 512;
        if (off < be && off + len > bs) {
            g_tb_fails++;
            if (g_tb_ms) usleep((useconds_t)g_tb_ms * 1000);
            return 1;
        }
    }
    return 0;
}
#endif

static int imgsrc_pread(void *src, void *buf, u32 len, u64 off);

static int dev_pread(Dev *d, void *buf, u32 len, u64 off)
{
    u8 *p = buf;
    if (d->src) return imgsrc_pread(d->src, buf, len, off);
#ifndef _WIN32
    if (g_tb_cnt != 0 && test_bad(d, off, len)) return -1;
#endif
#ifdef _WIN32
    if (!d->use_snap || !g_nsnap) return h_pread(d->h, buf, len, off);
#else
    if (!d->use_snap || !g_nsnap) return h_pread(d->fd, buf, len, off);
#endif
    /* divide a leitura: trechos dentro de um volume com snapshot vem do snapshot */
    while (len) {
#ifdef _WIN32
        fh_t h = d->h;
#else
        fh_t h = d->fd;
#endif
        u64 o = off;
        u32 n = len;
        int i;
        for (i = 0; i < g_nsnap; i++) {
            SnapMap *sm = &g_snap[i];
            if (!sm->h) continue;
            if (off >= sm->start && off < sm->start + sm->len) {
                h = sm->h;
                o = off - sm->start;
                if ((u64)n > sm->start + sm->len - off) n = (u32)(sm->start + sm->len - off);
                break;
            }
            if (sm->start > off && sm->start < off + n) n = (u32)(sm->start - off);
        }
        if (h_pread(h, p, n, o) != 0) return -1;
        p += n; off += n; len -= n;
    }
    return 0;
}

static int dev_pwrite(Dev *d, const void *buf, u32 len, u64 off)
{
#ifdef _WIN32
    LARGE_INTEGER li;
    DWORD put = 0;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(d->h, li, NULL, FILE_BEGIN)) return -1;
    if (!WriteFile(d->h, buf, len, &put, NULL) || put != len) return -1;
    return 0;
#else
    const u8 *p = buf;
    while (len) {
        ssize_t r = pwrite(d->fd, p, len, (off_t)off);
        if (r <= 0) return -1;
        p += r; off += (u64)r; len -= (u32)r;
    }
    return 0;
#endif
}

/* le qualquer faixa de bytes (cuida do alinhamento em setor) */
static int dev_read_any(Dev *d, u64 off, u64 len, void *out)
{
    u32 ss = d->sector;
    u8 *dst = out;
    u8 *tmp = xalloc(CHUNK_SIZE + 2 * 65536);
    int rc = 0;
    while (len) {
        u64 a = off - off % ss;
        u64 skip = off - a;
        u64 want = len < CHUNK_SIZE ? len : CHUNK_SIZE;
        u64 span = ((skip + want + ss - 1) / ss) * ss;
        if (dev_pread(d, tmp, (u32)span, a) != 0) { rc = -1; break; }
        memcpy(dst, tmp + skip, (size_t)want);
        dst += want; off += want; len -= want;
    }
    xfree(tmp);
    return rc;
}

/* grava qualquer faixa de bytes (le-modifica-grava setores parciais) */
static int dev_write_any(Dev *d, u64 off, u64 len, const void *in)
{
    u32 ss = d->sector;
    const u8 *src = in;
    u8 *tmp = xalloc(CHUNK_SIZE + 2 * 65536);
    int rc = 0;
    while (len) {
        u64 a = off - off % ss;
        u64 skip = off - a;
        u64 want = len < CHUNK_SIZE ? len : CHUNK_SIZE;
        u64 span = ((skip + want + ss - 1) / ss) * ss;
        if ((skip || (want % ss)) && dev_pread(d, tmp, (u32)span, a) != 0) { rc = -1; break; }
        memcpy(tmp + skip, src, (size_t)want);
        if (dev_pwrite(d, tmp, (u32)span, a) != 0) { rc = -1; break; }
        src += want; off += want; len -= want;
    }
    xfree(tmp);
    return rc;
}
