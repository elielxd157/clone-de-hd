/*
 * hdclone - clonagem de disco inteiro para imagem compactada (.hdc) e restauracao.
 *
 * Compativel com Windows XP ate Windows 11 (binario Win32 de 32 bits).
 * Tambem compila em Linux (usado para testes; aceita /dev/sdX ou arquivos).
 *
 * Formato .hdc:
 *   - Cabecalho de 512 bytes (magic "HDCLONE", tamanho do disco, setor, etc.)
 *   - Sequencia de blocos (chunks) de 1 MiB, cada um com:
 *       u8 tipo | u32 tamanho armazenado | u32 crc32 dos dados originais | dados
 *       tipo 0 = ZERO   (bloco todo zero, nada armazenado)
 *       tipo 1 = DEFLATE(comprimido com zlib)
 *       tipo 2 = RAW    (incompressivel, armazenado puro)
 *       tipo 3 = LIVRE  (espaco livre do sistema de arquivos, nada armazenado)
 *   - Rodape de 48 bytes ("HDCEND", estatisticas, crc32)
 *   - Pode ser dividido em partes (img.hdc, img.hdc.002, img.hdc.003...)
 *     para caber em pendrives FAT32 (limite de 4 GB por arquivo).
 *
 * Modo inteligente (padrao): le o bitmap do NTFS e a FAT do FAT32 para nao
 * copiar clusters livres. Outros sistemas de arquivos/areas sao copiados
 * setor a setor (com compressao), entao qualquer SO/particao e preservado.
 */

#ifdef _WIN32
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#define __USE_MINGW_ANSI_STDIO 1
#else
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <ctype.h>
#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#endif

#define VERSION_STR "hdclone 1.0"
#define CHUNK_SIZE (1024u * 1024u)
#define HDR_SIZE 512
#define FTR_SIZE 48
#define CH_ZERO 0
#define CH_DEFLATE 1
#define CH_RAW 2
#define CH_FREE 3

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

/* ------------------------------------------------------------------ */
/* utilitarios                                                         */
/* ------------------------------------------------------------------ */

static u16 le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u64 le64(const u8 *p) { return (u64)le32(p) | ((u64)le32(p + 4) << 32); }
static void put32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void put64(u8 *p, u64 v) { put32(p, (u32)v); put32(p + 4, (u32)(v >> 32)); }

static void die(const char *msg)
{
    fprintf(stderr, "\nERRO: %s\n", msg);
    exit(1);
}

static void *xalloc(size_t n)
{
    void *p;
#ifdef _WIN32
    /* memoria alinhada em pagina: exigida para I/O direto em disco */
    p = VirtualAlloc(NULL, n ? n : 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    if (posix_memalign(&p, 4096, n ? n : 1) != 0) p = NULL;
    else memset(p, 0, n);
#endif
    if (!p) die("memoria insuficiente");
    return p;
}

static void xfree(void *p)
{
    if (!p) return;
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    free(p);
#endif
}

static u64 now_ms(void)
{
#ifdef _WIN32
    return GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

#ifdef _WIN32
static int is_number(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}
#endif

static void fmt_size(u64 b, char *out)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    sprintf(out, "%.1f %s", v, u[i]);
}

static int all_zero(const u8 *p, u32 n)
{
    const u64 *q = (const u64 *)p;
    u32 i;
    for (i = 0; i < n / 8; i++)
        if (q[i]) return 0;
    for (i = (n / 8) * 8; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* acesso a disco / arquivo                                            */
/* ------------------------------------------------------------------ */

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
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
        CloseHandle(d->h);
    }
    d->h = NULL;
#else
    if (d->fd > 0) { fsync(d->fd); close(d->fd); }
    d->fd = -1;
#endif
}

static int dev_pread(Dev *d, void *buf, u32 len, u64 off)
{
#ifdef _WIN32
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(d->h, li, NULL, FILE_BEGIN)) return -1;
    if (!ReadFile(d->h, buf, len, &got, NULL) || got != len) return -1;
    return 0;
#else
    u8 *p = buf;
    while (len) {
        ssize_t r = pread(d->fd, p, len, (off_t)off);
        if (r <= 0) return -1;
        p += r; off += (u64)r; len -= (u32)r;
    }
    return 0;
#endif
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

/* ------------------------------------------------------------------ */
/* arquivos de imagem (com divisao em partes)                          */
/* ------------------------------------------------------------------ */

typedef struct {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
    char base[600];
    int part;
    u64 cur;   /* bytes na parte atual */
    u64 limit; /* tamanho maximo de cada parte (0 = sem limite) */
    u64 total;
} Img;

static void part_name(const char *base, int part, char *out)
{
    if (part <= 1) strcpy(out, base);
    else sprintf(out, "%s.%03d", base, part);
}

static int img_open_part(Img *m, int write)
{
    char nm[620];
    part_name(m->base, m->part, nm);
#ifdef _WIN32
    m->h = CreateFileA(nm, write ? GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ, NULL,
                       write ? CREATE_ALWAYS : OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    return m->h == INVALID_HANDLE_VALUE ? -1 : 0;
#else
    m->fd = open(nm, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0644);
    return m->fd < 0 ? -1 : 0;
#endif
}

static void img_close_part(Img *m)
{
#ifdef _WIN32
    if (m->h && m->h != INVALID_HANDLE_VALUE) CloseHandle(m->h);
    m->h = NULL;
#else
    if (m->fd > 0) close(m->fd);
    m->fd = -1;
#endif
}

static int img_open(Img *m, const char *path, int write, u64 limit)
{
    memset(m, 0, sizeof(*m));
    strncpy(m->base, path, sizeof(m->base) - 1);
    m->part = 1;
    m->limit = limit;
    return img_open_part(m, write);
}

static int raw_write(Img *m, const void *p, u32 n)
{
#ifdef _WIN32
    DWORD w;
    return (WriteFile(m->h, p, n, &w, NULL) && w == n) ? 0 : -1;
#else
    const u8 *q = p;
    while (n) {
        ssize_t r = write(m->fd, q, n);
        if (r <= 0) return -1;
        q += r; n -= (u32)r;
    }
    return 0;
#endif
}

static int raw_read(Img *m, void *p, u32 n)
{
#ifdef _WIN32
    DWORD r;
    if (!ReadFile(m->h, p, n, &r, NULL)) return -1;
    return (int)r;
#else
    ssize_t r = read(m->fd, p, n);
    return (int)r;
#endif
}

static void img_write(Img *m, const void *buf, u64 n)
{
    const u8 *p = buf;
    while (n) {
        u64 take = n;
        if (m->limit && m->cur >= m->limit) {
            img_close_part(m);
            m->part++;
            m->cur = 0;
            if (img_open_part(m, 1) != 0) die("nao foi possivel criar a proxima parte da imagem");
        }
        if (m->limit && take > m->limit - m->cur) take = m->limit - m->cur;
        if (raw_write(m, p, (u32)take) != 0)
            die("falha ao gravar imagem (disco de destino cheio?)");
        p += take; n -= take; m->cur += take; m->total += take;
    }
}

/* retorna 0 se leu tudo, -1 se acabou o arquivo/partes */
static int img_read(Img *m, void *buf, u64 n)
{
    u8 *p = buf;
    while (n) {
        u32 want = n > 0x40000000 ? 0x40000000 : (u32)n;
        int r = raw_read(m, p, want);
        if (r < 0) return -1;
        if (r == 0) {
            img_close_part(m);
            m->part++;
            if (img_open_part(m, 0) != 0) return -1;
            continue;
        }
        p += r; n -= (u64)r; m->total += (u64)r;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Windows: bloqueio de volumes, listagem de discos                    */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
#define MAX_LOCKS 128
static HANDLE g_locks[MAX_LOCKS];
static int g_nlocks;

static int volume_disk(HANDLE h, DWORD *disk)
{
    u8 buf[2048];
    DWORD br;
    VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)buf;
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof(buf), &br, NULL))
        return -1;
    if (e->NumberOfDiskExtents < 1) return -1;
    *disk = e->Extents[0].DiskNumber;
    return 0;
}

/* bloqueia (e opcionalmente desmonta) todos os volumes de um disco.
   retorna o numero de volumes que NAO puderam ser bloqueados. */
static int lock_disk_volumes(int disknum, int dismount)
{
    char vol[MAX_PATH], paths[1024];
    HANDLE fv = FindFirstVolumeA(vol, MAX_PATH);
    int fails = 0;
    if (fv == INVALID_HANDLE_VALUE) return 0;
    do {
        DWORD disk, br, plen = 0;
        HANDLE h;
        size_t n = strlen(vol);
        paths[0] = 0;
        GetVolumePathNamesForVolumeNameA(vol, paths, sizeof(paths), &plen);
        if (n && vol[n - 1] == '\\') vol[n - 1] = 0;
        h = CreateFileA(vol, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (volume_disk(h, &disk) == 0 && (int)disk == disknum) {
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL)) {
                if (dismount) DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &br, NULL);
                printf("  Volume %s bloqueado%s\n", paths[0] ? paths : "(sem letra)",
                       dismount ? " e desmontado" : "");
                if (g_nlocks < MAX_LOCKS) { g_locks[g_nlocks++] = h; continue; }
            } else {
                printf("  AVISO: volume %s esta em uso e nao pode ser bloqueado\n",
                       paths[0] ? paths : "(sem letra)");
                fails++;
            }
        }
        CloseHandle(h);
    } while (FindNextVolumeA(fv, vol, MAX_PATH));
    FindVolumeClose(fv);
    return fails;
}

static void unlock_all(void)
{
    int i;
    DWORD br;
    for (i = 0; i < g_nlocks; i++) {
        DeviceIoControl(g_locks[i], FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &br, NULL);
        CloseHandle(g_locks[i]);
    }
    g_nlocks = 0;
}

/* numero do disco fisico onde fica um caminho (arquivo/pasta), -1 se desconhecido */
static int path_disk(const char *path)
{
    char full[MAX_PATH], root[MAX_PATH], vol[MAX_PATH];
    HANDLE h;
    DWORD disk;
    size_t n;
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL)) return -1;
    if (!GetVolumePathNameA(full, root, MAX_PATH)) return -1;
    if (!GetVolumeNameForVolumeMountPointA(root, vol, MAX_PATH)) return -1;
    n = strlen(vol);
    if (n && vol[n - 1] == '\\') vol[n - 1] = 0;
    h = CreateFileA(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (volume_disk(h, &disk) != 0) disk = (DWORD)-1;
    CloseHandle(h);
    return (int)disk;
}

static int system_disk(void)
{
    char sys[MAX_PATH];
    if (!GetSystemDirectoryA(sys, MAX_PATH)) return -1;
    return path_disk(sys);
}

static void cmd_list(void)
{
    int n, found = 0, sysd = system_disk();
    printf("\nDiscos fisicos encontrados:\n\n");
    printf("  No.  Tamanho      Modelo / Volumes\n");
    printf("  ---  -----------  ------------------------------------------\n");
    for (n = 0; n < 32; n++) {
        char spec[16], sz[32], model[128] = "", letters[128] = "";
        Dev d;
        u8 buf[1024];
        DWORD br;
        STORAGE_PROPERTY_QUERY q;
        char l;
        sprintf(spec, "%d", n);
        if (dev_open(&d, spec, 0) != 0) continue;
        found++;
        memset(&q, 0, sizeof(q));
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        if (DeviceIoControl(d.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, NULL)) {
            STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
            if (sd->VendorIdOffset && sd->VendorIdOffset < br) {
                strncat(model, (char *)buf + sd->VendorIdOffset, 40);
                strcat(model, " ");
            }
            if (sd->ProductIdOffset && sd->ProductIdOffset < br)
                strncat(model, (char *)buf + sd->ProductIdOffset, 60);
        }
        for (l = 'A'; l <= 'Z'; l++) {
            char vp[8];
            HANDLE h;
            DWORD disk;
            sprintf(vp, "\\\\.\\%c:", l);
            h = CreateFileA(vp, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) continue;
            if (volume_disk(h, &disk) == 0 && (int)disk == n) {
                char t[4] = {l, ':', ' ', 0};
                strcat(letters, t);
            }
            CloseHandle(h);
        }
        fmt_size(d.size, sz);
        printf("  %-3d  %-11s  %s\n", n, sz, model[0] ? model : "(desconhecido)");
        printf("                    volumes: %s%s  setor: %u bytes\n",
               letters[0] ? letters : "(nenhum)", n == sysd ? " <- DISCO DO WINDOWS EM USO" : "",
               d.sector);
        dev_close(&d);
    }
    if (!found)
        printf("  Nenhum disco acessivel. Execute como Administrador.\n");
    printf("\n");
}
#else
static int lock_disk_volumes(int disknum, int dismount) { (void)disknum; (void)dismount; return 0; }
static void unlock_all(void) {}
static int path_disk(const char *p) { (void)p; return -1; }
static void cmd_list(void)
{
    printf("No Linux, use o caminho do dispositivo (ex.: /dev/sdb) ou um arquivo .img.\n"
           "Liste os discos com: lsblk\n");
}
#endif

/* ------------------------------------------------------------------ */
/* mapa de clusters usados (NTFS / FAT32)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u64 start; /* offset em bytes no disco do cluster 0 da area */
    u64 unit;  /* bytes por cluster */
    u64 count; /* numero de clusters */
    u8 *bm;    /* bit 1 = cluster usado */
} Area;

#define MAX_AREAS 128
static Area g_areas[MAX_AREAS];
static int g_nareas;

static int bm_get(const u8 *bm, u64 i) { return (bm[i >> 3] >> (i & 7)) & 1; }

static void add_area(u64 start, u64 unit, u64 count, u8 *bm)
{
    if (g_nareas >= MAX_AREAS) { free(bm); return; }
    g_areas[g_nareas].start = start;
    g_areas[g_nareas].unit = unit;
    g_areas[g_nareas].count = count;
    g_areas[g_nareas].bm = bm;
    g_nareas++;
}

static u64 count_used(const u8 *bm, u64 n)
{
    u64 i, c = 0;
    for (i = 0; i < n; i++) c += bm_get(bm, i);
    return c;
}

static void map_ntfs(Dev *d, u64 off, u64 plen, const u8 *b)
{
    u32 bps = le16(b + 11), spc, recsz, usa_off, usa_cnt, a, i;
    u8 spc_raw = b[13];
    signed char cpr = (signed char)b[0x40];
    u64 cs, tot_sec, mft_lcn, ncl, bmbytes, pos = 0;
    u8 *rec = NULL, *bm = NULL;
    char s1[32], s2[32];

    spc = spc_raw <= 0x80 ? spc_raw : (1u << (256 - spc_raw));
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc) goto fail;
    cs = (u64)bps * spc;
    tot_sec = le64(b + 0x28);
    mft_lcn = le64(b + 0x30);
    recsz = cpr > 0 ? (u32)(cpr * cs) : (1u << (-cpr));
    if (recsz < 256 || recsz > 65536) goto fail;
    ncl = tot_sec * bps / cs;
    if (ncl == 0 || ncl * cs > plen) goto fail;
    bmbytes = (ncl + 7) / 8;

    /* registro 6 da MFT = $Bitmap */
    rec = malloc(recsz);
    if (!rec || dev_read_any(d, off + mft_lcn * cs + 6ull * recsz, recsz, rec) != 0) goto fail;
    if (memcmp(rec, "FILE", 4) != 0) goto fail;
    usa_off = le16(rec + 4);
    usa_cnt = le16(rec + 6);
    for (i = 1; i < usa_cnt; i++) {
        u32 p = i * 512 - 2;
        if (p + 2 > recsz || usa_off + 2 * i + 1 >= recsz) break;
        if (rec[p] != rec[usa_off] || rec[p + 1] != rec[usa_off + 1]) goto fail;
        rec[p] = rec[usa_off + 2 * i];
        rec[p + 1] = rec[usa_off + 2 * i + 1];
    }
    a = le16(rec + 0x14);
    while (a + 16 <= recsz) {
        u32 type = le32(rec + a), alen = le32(rec + a + 4);
        if (type == 0xFFFFFFFF || alen < 16 || a + alen > recsz) { a = recsz; break; }
        if (type == 0x80 && rec[a + 9] == 0) break; /* $DATA sem nome */
        a += alen;
    }
    if (a + 16 > recsz) goto fail;

    bm = calloc(1, (size_t)bmbytes + 8);
    if (!bm) goto fail;
    if (rec[a + 8] == 0) { /* residente */
        u32 vlen = le32(rec + a + 0x10), voff = le16(rec + a + 0x14);
        if (a + voff + vlen > recsz) goto fail;
        memcpy(bm, rec + a + voff, vlen < bmbytes ? vlen : (size_t)bmbytes);
        pos = vlen;
    } else { /* nao residente: percorre a runlist */
        u32 alen = le32(rec + a + 4);
        u32 p = a + le16(rec + a + 0x20);
        i64 lcn = 0;
        while (p < a + alen && rec[p]) {
            u8 h = rec[p++];
            u32 ls = h & 15, os = h >> 4, k;
            u64 rl = 0, bytes;
            i64 delta = 0;
            if (ls == 0 || ls > 8 || os > 8 || p + ls + os > a + alen) goto fail;
            for (k = 0; k < ls; k++) rl |= (u64)rec[p + k] << (8 * k);
            p += ls;
            if (os) {
                u64 v = 0;
                for (k = 0; k < os; k++) v |= (u64)rec[p + k] << (8 * k);
                if (os < 8 && (rec[p + os - 1] & 0x80)) v |= ~0ull << (8 * os);
                delta = (i64)v;
                p += os;
            }
            bytes = rl * cs;
            if (os) {
                lcn += delta;
                if (pos < bmbytes) {
                    u64 want = bytes < bmbytes - pos ? bytes : bmbytes - pos;
                    if (dev_read_any(d, off + (u64)lcn * cs, want, bm + pos) != 0) goto fail;
                }
            }
            pos += bytes;
        }
    }
    if (pos < bmbytes) goto fail;
    /* verificacoes de sanidade: boot e MFT precisam estar marcados como usados */
    if (!bm_get(bm, 0) || mft_lcn >= ncl || !bm_get(bm, mft_lcn)) goto fail;

    fmt_size(ncl * cs, s1);
    fmt_size(count_used(bm, ncl) * cs, s2);
    printf("  NTFS em %llu MB: %s, usado %s (cluster %llu bytes)\n",
           (unsigned long long)(off >> 20), s1, s2, (unsigned long long)cs);
    add_area(off, cs, ncl, bm);
    free(rec);
    return;
fail:
    printf("  NTFS em %llu MB: nao foi possivel ler o bitmap, sera copiado inteiro\n",
           (unsigned long long)(off >> 20));
    free(rec);
    free(bm);
}

static void map_fat32(Dev *d, u64 off, u64 plen, const u8 *b)
{
    u32 bps = le16(b + 11), spc = b[13], rsv = le16(b + 14), nfats = b[16];
    u32 tot = le16(b + 19) ? le16(b + 19) : le32(b + 32), fatsz = le32(b + 36), root = le32(b + 44);
    u64 data_sec, ncl, cs, i, done = 0;
    u8 *bm = NULL, *buf = NULL;
    char s1[32], s2[32];

    if (bps < 512 || bps > 4096 || !spc || !nfats || !fatsz) goto fail;
    data_sec = rsv + (u64)nfats * fatsz;
    if (tot <= data_sec) goto fail;
    ncl = (tot - data_sec) / spc;
    cs = (u64)bps * spc;
    if ((u64)tot * bps > plen || (ncl + 2) * 4 > (u64)fatsz * bps) goto fail;
    bm = calloc(1, (size_t)(ncl + 7) / 8 + 8);
    buf = malloc(CHUNK_SIZE);
    if (!bm || !buf) goto fail;
    /* entradas da FAT para os clusters 2..ncl+1 */
    while (done < ncl) {
        u64 n = ncl - done, k;
        if (n > CHUNK_SIZE / 4) n = CHUNK_SIZE / 4;
        if (dev_read_any(d, off + (u64)rsv * bps + (done + 2) * 4, n * 4, buf) != 0) goto fail;
        for (k = 0; k < n; k++)
            if (le32(buf + k * 4) & 0x0FFFFFFF) bm[(done + k) >> 3] |= (u8)(1 << ((done + k) & 7));
        done += n;
    }
    if (root < 2 || root - 2 >= ncl || !bm_get(bm, root - 2)) goto fail;
    fmt_size(ncl * cs, s1);
    i = count_used(bm, ncl);
    fmt_size(i * cs, s2);
    printf("  FAT32 em %llu MB: %s, usado %s (cluster %llu bytes)\n",
           (unsigned long long)(off >> 20), s1, s2, (unsigned long long)cs);
    add_area(off + data_sec * bps, cs, ncl, bm);
    free(buf);
    return;
fail:
    printf("  FAT32 em %llu MB: nao foi possivel ler a FAT, sera copiado inteiro\n",
           (unsigned long long)(off >> 20));
    free(bm);
    free(buf);
}

static void probe_fs(Dev *d, u64 off, u64 len)
{
    u8 b[512];
    if (off + 512 > d->size || dev_read_any(d, off, 512, b) != 0) return;
    if (b[510] != 0x55 || b[511] != 0xAA) {
        printf("  Particao em %llu MB: sistema de arquivos nao reconhecido (copia setor a setor)\n",
               (unsigned long long)(off >> 20));
        return;
    }
    if (memcmp(b + 3, "NTFS    ", 8) == 0) map_ntfs(d, off, len, b);
    else if (memcmp(b + 82, "FAT32   ", 8) == 0) map_fat32(d, off, len, b);
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
            int st = bm_get(A->bm, c);
            while (ce < end && c + 1 < A->count && bm_get(A->bm, c + 1) == st) { c++; ce += A->unit; }
            *is_free = !st;
            return ce < end ? ce : end;
        }
        if (A->start > pos && A->start < nx) nx = A->start;
    }
    *is_free = 0;
    return nx;
}

/* ------------------------------------------------------------------ */
/* progresso                                                           */
/* ------------------------------------------------------------------ */

static u64 g_t0, g_tlast;

static void progress(u64 done, u64 total, int force)
{
    u64 t = now_ms(), el;
    double pct, speed;
    unsigned long eta;
    if (!force && t - g_tlast < 500) return;
    g_tlast = t;
    el = t - g_t0;
    if (!el) el = 1;
    pct = total ? 100.0 * (double)done / (double)total : 100.0;
    speed = ((double)done / 1048576.0) / ((double)el / 1000.0);
    eta = (speed > 0 && done < total) ? (unsigned long)(((double)(total - done) / 1048576.0) / speed) : 0;
    printf("\r  %5.1f%%  %llu / %llu MB  %6.1f MB/s  restante %02lu:%02lu:%02lu ",
           pct, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20), speed,
           eta / 3600, (eta / 60) % 60, eta % 60);
    fflush(stdout);
}

static int confirm(const char *msg)
{
    char line[64];
    printf("%s\nDigite SIM para continuar: ", msg);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin)) return 0;
    return strncmp(line, "SIM", 3) == 0 || strncmp(line, "sim", 3) == 0;
}

/* ------------------------------------------------------------------ */
/* BACKUP                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int level;
    u64 split;
    int raw;
    int yes;
    int force;
    int zero_free;
} Opts;

static void cmd_backup(const char *src, const char *dst, Opts *o)
{
    Dev d;
    Img m;
    u8 hdr[HDR_SIZE], ftr[FTR_SIZE], ch[9];
    u8 *buf, *cbuf;
    uLong cbound = compressBound(CHUNK_SIZE);
    u64 nchunks, i, used = 0, bad = 0, nfree = 0;
    char s1[32], s2[32];

    if (dev_open(&d, src, 0) != 0) die("nao foi possivel abrir o disco de origem (execute como Administrador)");
    if (!d.size) die("nao foi possivel obter o tamanho do disco de origem");
    fmt_size(d.size, s1);
    printf("\nOrigem : %s  (%s, setor %u bytes)\n", d.name, s1, d.sector);
    printf("Destino: %s\n", dst);

    if (d.disknum >= 0 && path_disk(dst) == d.disknum)
        die("a imagem nao pode ser gravada no mesmo disco que esta sendo copiado");

    if (!o->raw) {
        printf("\nAnalisando particoes...\n");
        scan_partitions(&d);
    } else {
        printf("\nModo setor a setor (--raw): todo o disco sera copiado\n");
    }

    if (d.disknum >= 0) {
        printf("\nBloqueando volumes da origem...\n");
        if (lock_disk_volumes(d.disknum, 0) > 0)
            printf("  AVISO: ha volumes em uso. Arquivos abertos podem ficar inconsistentes na imagem.\n"
                   "  Para um clone 100%% consistente do disco do Windows, rode o hdclone a partir\n"
                   "  de um boot externo (WinPE / Hiren's BootCD PE) ou com o HD em outro PC.\n");
    }

    if (img_open(&m, dst, 1, o->split) != 0) die("nao foi possivel criar o arquivo de imagem");

    nchunks = (d.size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "HDCLONE", 8);
    put32(hdr + 8, 1);
    put32(hdr + 12, d.sector);
    put32(hdr + 16, CHUNK_SIZE);
    put32(hdr + 20, o->raw ? 0 : 1);
    put64(hdr + 24, d.size);
    put64(hdr + 32, nchunks);
    put64(hdr + 40, (u64)time(NULL));
    memcpy(hdr + 48, d.name, strlen(d.name) < 127 ? strlen(d.name) : 127);
    strncpy((char *)hdr + 176, VERSION_STR, 31);
    put32(hdr + 508, (u32)crc32(0, hdr, 508));
    img_write(&m, hdr, HDR_SIZE);

    buf = xalloc(CHUNK_SIZE);
    cbuf = xalloc(cbound);
    printf("\nCopiando...\n");
    g_t0 = g_tlast = now_ms();

    for (i = 0; i < nchunks; i++) {
        u64 off = i * CHUNK_SIZE;
        u32 len = (u32)((d.size - off) < CHUNK_SIZE ? (d.size - off) : CHUNK_SIZE);
        int any_free = 0, all_free = 1, f;
        u64 p, q;
        u8 type;
        u32 stored = 0, crc = 0;

        if (!o->raw && g_nareas) {
            for (p = off; p < off + len; p = q) {
                q = seg_next(p, off + len, &f);
                if (f) any_free = 1; else all_free = 0;
            }
        } else {
            all_free = 0;
        }

        if (all_free) {
            type = CH_FREE;
            nfree++;
        } else {
            if (dev_pread(&d, buf, len, off) != 0) {
                /* setor defeituoso: tenta setor por setor */
                u32 k;
                for (k = 0; k < len; k += d.sector) {
                    if (dev_pread(&d, buf + k, d.sector, off + k) != 0) {
                        memset(buf + k, 0, d.sector);
                        bad++;
                    }
                }
            }
            if (any_free) { /* zera os clusters livres para comprimir melhor */
                for (p = off; p < off + len; p = q) {
                    q = seg_next(p, off + len, &f);
                    if (f) memset(buf + (p - off), 0, (size_t)(q - p));
                }
            }
            if (all_zero(buf, len)) {
                type = CH_ZERO;
            } else {
                uLongf cl = cbound;
                used += len;
                crc = (u32)crc32(0, buf, len);
                if (compress2(cbuf, &cl, buf, len, o->level) == Z_OK && cl < len) {
                    type = CH_DEFLATE;
                    stored = (u32)cl;
                } else {
                    type = CH_RAW;
                    stored = len;
                }
            }
        }
        ch[0] = type;
        put32(ch + 1, stored);
        put32(ch + 5, crc);
        img_write(&m, ch, 9);
        if (type == CH_DEFLATE) img_write(&m, cbuf, stored);
        else if (type == CH_RAW) img_write(&m, buf, stored);
        progress(off + len, d.size, i + 1 == nchunks);
    }

    memset(ftr, 0, sizeof(ftr));
    memcpy(ftr, "HDCEND", 6);
    put64(ftr + 8, nchunks);
    put64(ftr + 16, used);
    put64(ftr + 24, m.total + FTR_SIZE);
    put64(ftr + 32, bad);
    put32(ftr + 44, (u32)crc32(0, ftr, 44));
    img_write(&m, ftr, FTR_SIZE);
    img_close_part(&m);
    unlock_all();
    dev_close(&d);

    fmt_size(d.size, s1);
    fmt_size(m.total, s2);
    printf("\n\nBackup concluido!\n");
    printf("  Disco: %s   Imagem: %s (%.1f%% do disco)   Partes: %d\n", s1, s2,
           100.0 * (double)m.total / (double)d.size, m.part);
    if (nfree) printf("  Blocos de espaco livre ignorados: %llu MB\n", (unsigned long long)nfree);
    if (bad) printf("  ATENCAO: %llu setores ilegiveis foram preenchidos com zeros!\n", (unsigned long long)bad);
    xfree(buf);
    xfree(cbuf);
}

/* ------------------------------------------------------------------ */
/* leitura do cabecalho                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 sector, chunk, flags;
    u64 size, nchunks, created;
    char src[129], tool[33];
} Hdr;

static void read_header(Img *m, Hdr *h)
{
    u8 b[HDR_SIZE];
    if (img_read(m, b, HDR_SIZE) != 0) die("arquivo de imagem vazio ou ilegivel");
    if (memcmp(b, "HDCLONE", 8) != 0) die("este arquivo nao e uma imagem do hdclone");
    if (le32(b + 508) != (u32)crc32(0, b, 508)) die("cabecalho da imagem corrompido");
    if (le32(b + 8) != 1) die("versao de imagem nao suportada");
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
    printf("  Modo           : %s\n", (h->flags & 1) ? "inteligente (somente dados usados)" : "setor a setor");
}

/* ------------------------------------------------------------------ */
/* VERIFY / RESTORE                                                    */
/* ------------------------------------------------------------------ */

/* corrige a GPT quando o disco de destino e maior que o original */
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

/* processa a imagem; se d != NULL grava no disco */
static void process_image(const char *path, Dev *d, Opts *o, Hdr *h, Img *m)
{
    u8 ch[9], ftr[FTR_SIZE];
    u8 *buf = xalloc(CHUNK_SIZE), *cbuf = xalloc(compressBound(CHUNK_SIZE)), *zeros = xalloc(CHUNK_SIZE);
    u64 i;
    (void)path;
    g_t0 = g_tlast = now_ms();
    for (i = 0; i < h->nchunks; i++) {
        u64 off = i * CHUNK_SIZE;
        u32 len = (u32)((h->size - off) < CHUNK_SIZE ? (h->size - off) : CHUNK_SIZE);
        u32 stored, crc;
        const u8 *data = NULL;
        char msg[128];
        if (img_read(m, ch, 9) != 0) {
            sprintf(msg, "imagem incompleta: termina no bloco %llu de %llu (falta alguma parte?)",
                    (unsigned long long)i, (unsigned long long)h->nchunks);
            die(msg);
        }
        stored = le32(ch + 1);
        crc = le32(ch + 5);
        switch (ch[0]) {
        case CH_ZERO:
            data = zeros;
            break;
        case CH_FREE:
            data = o->zero_free ? zeros : NULL;
            break;
        case CH_DEFLATE: {
            uLongf ul = CHUNK_SIZE;
            if (stored > compressBound(CHUNK_SIZE) || img_read(m, cbuf, stored) != 0) die("imagem corrompida/incompleta");
            if (uncompress(buf, &ul, cbuf, stored) != Z_OK || ul != len) {
                sprintf(msg, "dados corrompidos no bloco %llu", (unsigned long long)i);
                die(msg);
            }
            data = buf;
            break;
        }
        case CH_RAW:
            if (stored != len || img_read(m, buf, stored) != 0) die("imagem corrompida/incompleta");
            data = buf;
            break;
        default:
            sprintf(msg, "tipo de bloco invalido no bloco %llu (imagem corrompida)", (unsigned long long)i);
            die(msg);
        }
        if ((ch[0] == CH_DEFLATE || ch[0] == CH_RAW) && (u32)crc32(0, data, len) != crc) {
            sprintf(msg, "CRC invalido no bloco %llu (imagem corrompida)", (unsigned long long)i);
            die(msg);
        }
        if (d && data && dev_pwrite(d, data, len, off) != 0) {
            sprintf(msg, "falha de escrita no disco de destino na posicao %llu MB", (unsigned long long)(off >> 20));
            die(msg);
        }
        progress(off + len, h->size, i + 1 == h->nchunks);
    }
    if (img_read(m, ftr, FTR_SIZE) != 0 || memcmp(ftr, "HDCEND", 6) != 0 ||
        le32(ftr + 44) != (u32)crc32(0, ftr, 44))
        die("rodape da imagem ausente ou corrompido (backup incompleto?)");
    if (le64(ftr + 32))
        printf("\n  Obs.: o disco original tinha %llu setores ilegiveis (gravados como zero).",
               (unsigned long long)le64(ftr + 32));
    xfree(buf);
    xfree(cbuf);
    xfree(zeros);
}

static void cmd_info(const char *path)
{
    Img m;
    Hdr h;
    if (img_open(&m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&m, &h);
    printf("\nImagem: %s\n", path);
    print_header(&h);
    img_close_part(&m);
}

static void cmd_verify(const char *path)
{
    Img m;
    Hdr h;
    Opts o;
    memset(&o, 0, sizeof(o));
    if (img_open(&m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&m, &h);
    printf("\nVerificando %s\n", path);
    print_header(&h);
    printf("\n");
    process_image(path, NULL, &o, &h, &m);
    img_close_part(&m);
    printf("\n\nImagem OK: todos os blocos conferem (CRC32).\n");
}

static void cmd_restore(const char *path, const char *dst, Opts *o)
{
    Img m;
    Hdr h;
    Dev d;
    char s1[32], s2[32], msg[800];

    if (img_open(&m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&m, &h);
    printf("\nImagem: %s\n", path);
    print_header(&h);

    if (dev_open(&d, dst, 1) != 0) die("nao foi possivel abrir o disco de destino (execute como Administrador)");
    if (d.disknum >= 0 && path_disk(path) == d.disknum)
        die("a imagem esta no proprio disco de destino; copie-a para outro disco antes");
    fmt_size(h.size, s1);
    fmt_size(d.size, s2);
    printf("\nDestino: %s (%s)\n", d.name, s2);

    if (d.is_device) {
        if (d.size < h.size) {
            sprintf(msg, "o disco de destino (%s) e menor que o disco original (%s)", s2, s1);
            die(msg);
        }
        if (d.sector != h.sector) {
            printf("\nAVISO: tamanho de setor diferente (origem %u, destino %u). As particoes\n"
                   "provavelmente NAO funcionarao no destino.\n", h.sector, d.sector);
            if (!o->force) die("use --force para restaurar mesmo assim");
        }
    }

    if (!o->yes) {
        sprintf(msg, "\nATENCAO: TODO o conteudo de %s sera APAGADO e substituido pela imagem!", d.name);
        if (!confirm(msg)) { printf("Cancelado.\n"); exit(1); }
    }

    if (d.disknum >= 0) {
        printf("\nDesmontando volumes do destino...\n");
        if (lock_disk_volumes(d.disknum, 1) > 0 && !o->force)
            die("ha volumes em uso no disco de destino (feche programas/janelas ou use --force)");
    }

    printf("\nRestaurando...\n");
    process_image(path, &d, o, &h, &m);
    img_close_part(&m);

    if (!d.is_device) {
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)h.size;
        if (d.size < h.size) { SetFilePointerEx(d.h, li, NULL, FILE_BEGIN); SetEndOfFile(d.h); }
#else
        if (d.size < h.size && ftruncate(d.fd, (off_t)h.size) != 0) die("falha ao ajustar tamanho do arquivo");
#endif
        d.size = d.size > h.size ? d.size : h.size;
    }
    printf("\n\nFinalizando...\n");
    if (d.size > h.size) fix_gpt(&d);
#ifdef _WIN32
    if (d.is_device) {
        DWORD br;
        FlushFileBuffers(d.h);
        DeviceIoControl(d.h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);
    }
#endif
    unlock_all();
    dev_close(&d);
    printf("Restauracao concluida!\n");
    if (d.size > h.size)
        printf("Obs.: o disco de destino e maior. O espaco extra ficou nao alocado; use o\n"
               "Gerenciamento de Disco (Estender volume) ou diskpart para aproveita-lo.\n");
}

/* ------------------------------------------------------------------ */
/* interface                                                           */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf(
        "\n" VERSION_STR " - clone de HD para imagem compactada\n\n"
        "Uso:\n"
        "  hdclone list                         lista os discos fisicos\n"
        "  hdclone backup  <disco> <img.hdc>    cria imagem do disco\n"
        "  hdclone restore <img.hdc> <disco>    grava a imagem em um disco\n"
        "  hdclone verify  <img.hdc>            confere a integridade da imagem\n"
        "  hdclone info    <img.hdc>            mostra informacoes da imagem\n\n"
        "<disco> = numero mostrado em 'list' (ex.: 1) ou caminho de arquivo/dispositivo.\n\n"
        "Opcoes de backup:\n"
        "  -c N         nivel de compressao 1 (rapido) a 9 (menor). Padrao: 6\n"
        "  -s MB        dividir a imagem em partes de MB megabytes. Padrao: 4000\n"
        "               (cabe em pendrive FAT32). Use -s 0 para arquivo unico.\n"
        "  --raw        copia setor a setor, inclusive espaco livre\n"
        "Opcoes de restore:\n"
        "  -y           nao pedir confirmacao\n"
        "  --zero-free  grava zeros no espaco livre (mais lento, apaga dados antigos)\n"
        "  --force      ignora avisos (volumes em uso, setor diferente)\n\n"
        "Exemplos:\n"
        "  hdclone backup 1 E:\\backup\\pc-sala.hdc\n"
        "  hdclone restore E:\\backup\\pc-sala.hdc 2\n\n"
        "Execute como Administrador. Ao clonar o disco do Windows em uso, prefira\n"
        "iniciar por WinPE/Hiren's BootCD PE ou ligar o HD em outro computador.\n");
}

static void read_line(const char *prompt, char *out, int n)
{
    size_t l;
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, n, stdin)) { out[0] = 0; return; }
    l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
    /* remove aspas (arrastar arquivo para o console coloca aspas) */
    if (l >= 2 && out[0] == '"' && out[l - 1] == '"') { memmove(out, out + 1, l - 2); out[l - 2] = 0; }
}

static void interactive(Opts *o)
{
    char op[16], a[600], b[600];
    for (;;) {
        printf("\n===== " VERSION_STR " =====\n"
               " 1) Listar discos\n 2) Fazer backup (disco -> imagem)\n"
               " 3) Restaurar (imagem -> disco)\n 4) Verificar imagem\n 0) Sair\nOpcao: ");
        fflush(stdout);
        if (!fgets(op, sizeof(op), stdin)) return;
        switch (op[0]) {
        case '1': cmd_list(); break;
        case '2':
            cmd_list();
            read_line("Numero do disco de ORIGEM: ", a, sizeof(a));
            read_line("Arquivo de imagem a criar (ex.: E:\\backup.hdc): ", b, sizeof(b));
            if (a[0] && b[0]) cmd_backup(a, b, o);
            break;
        case '3':
            read_line("Arquivo de imagem (.hdc): ", a, sizeof(a));
            cmd_list();
            read_line("Numero do disco de DESTINO: ", b, sizeof(b));
            if (a[0] && b[0]) cmd_restore(a, b, o);
            break;
        case '4':
            read_line("Arquivo de imagem (.hdc): ", a, sizeof(a));
            if (a[0]) cmd_verify(a);
            break;
        case '0': return;
        }
    }
}

int main(int argc, char **argv)
{
    Opts o;
    char *pos[3];
    int np = 0, i;
    memset(&o, 0, sizeof(o));
    o.level = 6;
    o.split = 4000ull * 1024 * 1024;

    if (argc < 2) {
        interactive(&o);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            o.level = atoi(argv[++i]);
            if (o.level < 1 || o.level > 9) die("nivel de compressao deve ser de 1 a 9");
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            const char *q = argv[++i];
            u64 v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + (u64)(*q++ - '0');
            o.split = v * 1024 * 1024;
        } else if (!strcmp(argv[i], "--raw")) o.raw = 1;
        else if (!strcmp(argv[i], "-y")) o.yes = 1;
        else if (!strcmp(argv[i], "--force")) o.force = 1;
        else if (!strcmp(argv[i], "--zero-free")) o.zero_free = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "/?")) { usage(); return 0; }
        else if (np < 3) pos[np++] = argv[i];
        else { usage(); return 1; }
    }
    if (np == 1 && !strcmp(pos[0], "list")) cmd_list();
    else if (np == 3 && !strcmp(pos[0], "backup")) cmd_backup(pos[1], pos[2], &o);
    else if (np == 3 && !strcmp(pos[0], "restore")) cmd_restore(pos[1], pos[2], &o);
    else if (np == 2 && !strcmp(pos[0], "verify")) cmd_verify(pos[1]);
    else if (np == 2 && !strcmp(pos[0], "info")) cmd_info(pos[1]);
    else { usage(); return 1; }
    return 0;
}
