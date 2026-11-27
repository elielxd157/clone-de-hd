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
#include <objbase.h>
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

/* registro de recursos: permite liberar tudo se a operacao for abortada
   (na interface grafica um erro encerra so a operacao, nao o programa) */
#define RES_MEM 1
#define RES_HANDLE 2
#define MAX_RES 64
static struct { int kind; void *p; } g_res[MAX_RES];
static volatile int g_cancel;

static void res_add(int kind, void *p)
{
    int i;
    for (i = 0; i < MAX_RES; i++)
        if (!g_res[i].p) { g_res[i].kind = kind; g_res[i].p = p; return; }
}

static void res_del(void *p)
{
    int i;
    for (i = 0; i < MAX_RES; i++)
        if (g_res[i].p == p) { g_res[i].p = NULL; return; }
}

#ifdef HDCLONE_GUI
static void ui_log(const char *fmt, ...);
static void ui_fatal(const char *msg);
static void ui_progress(u64 done, u64 total, double speed, unsigned long eta);
#define printf ui_log
#endif

static void die(const char *msg)
{
#ifdef HDCLONE_GUI
    ui_fatal(msg); /* nao retorna */
#else
    fprintf(stderr, "\nERRO: %s\n", msg);
#endif
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
    res_add(RES_MEM, p);
    return p;
}

static void xfree(void *p)
{
    if (!p) return;
    res_del(p);
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

static int imgsrc_pread(void *src, void *buf, u32 len, u64 off);

static int dev_pread(Dev *d, void *buf, u32 len, u64 off)
{
    u8 *p = buf;
    if (d->src) return imgsrc_pread(d->src, buf, len, off);
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
    u64 pos;   /* leitura: posicao na parte atual */
    u64 psize; /* leitura: tamanho da parte atual */
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
    if (m->h == INVALID_HANDLE_VALUE) return -1;
    res_add(RES_HANDLE, m->h);
    m->pos = 0;
    {
        LARGE_INTEGER sz;
        m->psize = GetFileSizeEx(m->h, &sz) ? (u64)sz.QuadPart : 0;
    }
    return 0;
#else
    struct stat st;
    m->fd = open(nm, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0644);
    if (m->fd < 0) return -1;
    res_add(RES_HANDLE, (void *)(intptr_t)(m->fd + 1));
    m->pos = 0;
    m->psize = fstat(m->fd, &st) == 0 ? (u64)st.st_size : 0;
    return 0;
#endif
}

static void img_close_part(Img *m)
{
#ifdef _WIN32
    if (m->h && m->h != INVALID_HANDLE_VALUE) { res_del(m->h); CloseHandle(m->h); }
    m->h = NULL;
#else
    if (m->fd > 0) { res_del((void *)(intptr_t)(m->fd + 1)); close(m->fd); }
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
        p += r; n -= (u64)r; m->total += (u64)r; m->pos += (u64)r;
    }
    return 0;
}

/* posiciona a leitura em (parte, posicao) */
static int img_seek(Img *m, int part, u64 pos)
{
    if (m->part != part) {
        img_close_part(m);
        m->part = part;
        if (img_open_part(m, 0) != 0) return -1;
    }
#ifdef _WIN32
    {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)pos;
        if (!SetFilePointerEx(m->h, li, NULL, FILE_BEGIN)) return -1;
    }
#else
    if (lseek(m->fd, (off_t)pos, SEEK_SET) < 0) return -1;
#endif
    m->pos = pos;
    return 0;
}

/* pula n bytes sem ler (atravessa partes) */
static int img_skip(Img *m, u64 n)
{
    while (n) {
        u64 rem = m->psize > m->pos ? m->psize - m->pos : 0;
        if (n <= rem) return img_seek(m, m->part, m->pos + n);
        n -= rem;
        if (img_seek(m, m->part + 1, 0) != 0) return -1;
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
        {
            int k, snap = 0;
            for (k = 0; k < g_nsnap; k++)
                if (g_snap[k].h && !_stricmp(g_snap[k].vol, vol)) snap = 1;
            if (snap) continue; /* ja protegido pelo snapshot VSS */
        }
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

/* ------------------------------------------------------------------ */
/* VSS (Volume Shadow Copy): copia consistente com o Windows ligado    */
/* ------------------------------------------------------------------ */
/*
 * A interface IVssBackupComponents muda de layout entre o Windows XP e o
 * Vista+ (mesmo nome, metodos diferentes), por isso os metodos sao chamados
 * pela posicao na vtable, com uma tabela para cada versao. vssapi.dll e
 * carregada dinamicamente (no XP a funcao exportada tem outro nome).
 * Programas de 32 bits nao podem usar VSS no Windows de 64 bits: use as
 * versoes 64 (o hdclone de 32 bits as chama automaticamente).
 */

typedef struct {
    GUID SnapshotId, SnapshotSetId;
    LONG SnapshotsCount;
    WCHAR *SnapshotDeviceObject, *OriginalVolumeName, *OriginatingMachine;
    WCHAR *ServiceMachine, *ExposedName, *ExposedPath;
    GUID ProviderId;
    LONG SnapshotAttributes;
    LONGLONG CreationTimestamp;
    int Status;
} VssSnapProp;

typedef struct {
    int init, setstate, gather, prepare, abort, complete, start, add, doset, props;
    int wait_arg; /* IVssAsync::Wait recebe timeout (Vista+) ou nada (XP) */
} VssIdx;

static const VssIdx VSS_XP = {5, 6, 8, 13, 14, 26, 33, 34, 35, 40, 0};
static const VssIdx VSS_VISTA = {5, 6, 9, 14, 15, 27, 36, 37, 38, 42, 1};

typedef HRESULT(STDMETHODCALLTYPE *vf_bstr)(void *, BSTR);
typedef HRESULT(STDMETHODCALLTYPE *vf_state)(void *, int, int, int, int);
typedef HRESULT(STDMETHODCALLTYPE *vf_async)(void *, void **);
typedef HRESULT(STDMETHODCALLTYPE *vf_void)(void *);
typedef HRESULT(STDMETHODCALLTYPE *vf_guidp)(void *, GUID *);
typedef HRESULT(STDMETHODCALLTYPE *vf_add)(void *, WCHAR *, GUID, GUID *);
typedef HRESULT(STDMETHODCALLTYPE *vf_props)(void *, GUID, VssSnapProp *);
typedef HRESULT(STDMETHODCALLTYPE *vf_wait)(void *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *vf_qs)(void *, HRESULT *, INT *);
typedef ULONG(STDMETHODCALLTYPE *vf_rel)(void *);
typedef HRESULT(STDAPICALLTYPE *vf_create)(void **);

#define VM(obj, idx, type) ((type)((*(void ***)(obj))[idx]))

static void *g_vss;
static const VssIdx *g_vidx;
static int g_vss_com;
static char g_vss_note[300]; /* motivo quando o VSS nao pode ser usado */

static int is_wow64(void)
{
    typedef BOOL(WINAPI * fw)(HANDLE, PBOOL);
    BOOL w = FALSE;
    fw f = (fw)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    if (f) f(GetCurrentProcess(), &w);
    return w ? 1 : 0;
}

/* seguranca COM exigida pelo VSS; deve ser chamada cedo (uma vez por processo) */
static void vss_security(void)
{
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                         RPC_C_IMP_LEVEL_IDENTIFY, NULL, EOAC_NONE, NULL);
}

static HRESULT vss_wait(void *async)
{
    HRESULT hr, res = E_FAIL;
    if (g_vidx->wait_arg) hr = VM(async, 4, vf_wait)(async, INFINITE);
    else hr = VM(async, 4, vf_void)(async);
    if (SUCCEEDED(hr)) {
        hr = VM(async, 5, vf_qs)(async, &res, NULL);
        if (SUCCEEDED(hr)) hr = res;
    }
    VM(async, 2, vf_rel)(async);
    return hr;
}

static const char *vss_errtext(HRESULT hr)
{
    switch ((unsigned long)hr) {
    case 0x80042306: return "o provedor de snapshots esta ocupado; tente de novo";
    case 0x8004230C: return "volume nao suporta snapshot";
    case 0x8004230F: return "erro no provedor de snapshots";
    case 0x80042312: return "limite de snapshots atingido";
    case 0x80042316: return "ja existe outro snapshot sendo criado; tente de novo em alguns minutos";
    case 0x80042317: return "limite de snapshots do volume atingido";
    case 0x8004231F: return "espaco insuficiente para o snapshot (deixe ao menos ~10%% livre no volume)";
    case 0x80042302: return "componente VSS com problema (servico 'Copias de Sombra de Volume')";
    case 0x80070005: return "acesso negado (execute como Administrador)";
    case 0x80070424: return "servico de Copias de Sombra de Volume desativado";
    default: return "erro do VSS";
    }
}

static void vss_end(int ok)
{
    int i;
    for (i = 0; i < g_nsnap; i++)
        if (g_snap[i].h) { CloseHandle(g_snap[i].h); g_snap[i].h = NULL; }
    g_nsnap = 0;
    if (g_vss) {
        void *a = NULL;
        if (ok && SUCCEEDED(VM(g_vss, g_vidx->complete, vf_async)(g_vss, &a)) && a) vss_wait(a);
        else if (!ok) VM(g_vss, g_vidx->abort, vf_void)(g_vss);
        VM(g_vss, 2, vf_rel)(g_vss); /* libera: snapshots temporarios sao apagados */
        g_vss = NULL;
    }
    if (g_vss_com) { CoUninitialize(); g_vss_com = 0; }
}

#define VSS_FAIL(what, hr)                                                                  \
    do {                                                                                    \
        sprintf(g_vss_note, "%s: %s (0x%08lX)", what, vss_errtext(hr), (unsigned long)(hr)); \
        vss_end(0);                                                                         \
        return -1;                                                                          \
    } while (0)

/* cria snapshots de todos os volumes do disco; 0 = ok, -1 = nao foi possivel */
static int vss_begin(int disknum)
{
    OSVERSIONINFOA ov;
    HMODULE dll;
    vf_create create;
    HRESULT hr;
    void *a = NULL;
    GUID setid, snapid[MAX_SNAPS];
    char vol[MAX_PATH];
    HANDLE fv;
    int i, n = 0;

    g_vss_note[0] = 0;
    g_nsnap = 0;
    memset(&ov, 0, sizeof(ov));
    ov.dwOSVersionInfoSize = sizeof(ov);
    GetVersionExA(&ov);
    if (ov.dwMajorVersion == 5 && ov.dwMinorVersion == 1) g_vidx = &VSS_XP;
    else if (ov.dwMajorVersion >= 6) g_vidx = &VSS_VISTA;
    else { strcpy(g_vss_note, "VSS nao suportado nesta versao do Windows"); return -1; }
    if (is_wow64()) {
        strcpy(g_vss_note, "Windows de 64 bits: o VSS exige a versao 64 bits (hdclone64.exe / hdclone-gui64.exe)");
        return -1;
    }

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) g_vss_com = 1;
    vss_security();

    dll = LoadLibraryA("vssapi.dll");
    if (!dll) { strcpy(g_vss_note, "vssapi.dll nao encontrada"); vss_end(0); return -1; }
    create = (vf_create)(void *)GetProcAddress(dll, "CreateVssBackupComponentsInternal");
#ifdef _WIN64
    if (!create) create = (vf_create)(void *)GetProcAddress(dll, "?CreateVssBackupComponents@@YAJPEAPEAVIVssBackupComponents@@@Z");
#else
    if (!create) create = (vf_create)(void *)GetProcAddress(dll, "?CreateVssBackupComponents@@YGJPAPAVIVssBackupComponents@@@Z");
#endif
    if (!create) { strcpy(g_vss_note, "funcao VSS nao encontrada em vssapi.dll"); vss_end(0); return -1; }

    printf("  Iniciando VSS (copia de sombra)...\n");
    hr = create(&g_vss);
    if (FAILED(hr) || !g_vss) { g_vss = NULL; VSS_FAIL("CreateVssBackupComponents", hr); }
    hr = VM(g_vss, g_vidx->init, vf_bstr)(g_vss, NULL);
    if (FAILED(hr)) VSS_FAIL("InitializeForBackup", hr);
    /* VSS_BT_COPY (5): nao interfere no historico de backups de SQL/Exchange etc. */
    hr = VM(g_vss, g_vidx->setstate, vf_state)(g_vss, 0, 0, 5, 0);
    if (FAILED(hr)) VSS_FAIL("SetBackupState", hr);
    hr = VM(g_vss, g_vidx->gather, vf_async)(g_vss, &a);
    if (FAILED(hr) || FAILED(hr = vss_wait(a))) VSS_FAIL("GatherWriterMetadata", hr);
    hr = VM(g_vss, g_vidx->start, vf_guidp)(g_vss, &setid);
    if (FAILED(hr)) VSS_FAIL("StartSnapshotSet", hr);

    /* adiciona cada volume que fica neste disco */
    fv = FindFirstVolumeA(vol, MAX_PATH);
    if (fv != INVALID_HANDLE_VALUE) {
        do {
            char paths[256] = "";
            DWORD plen = 0;
            u8 buf[2048];
            DWORD br;
            HANDLE h;
            VOLUME_DISK_EXTENTS *e = (VOLUME_DISK_EXTENTS *)buf;
            WCHAR wvol[MAX_PATH];
            size_t l = strlen(vol);
            if (n >= MAX_SNAPS) break;
            GetVolumePathNamesForVolumeNameA(vol, paths, sizeof(paths), &plen);
            if (l && vol[l - 1] == '\\') vol[l - 1] = 0;
            h = CreateFileA(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) continue;
            if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, buf, sizeof(buf), &br, NULL) ||
                e->NumberOfDiskExtents < 1 || (int)e->Extents[0].DiskNumber != disknum) {
                CloseHandle(h);
                continue;
            }
            CloseHandle(h);
            if (e->NumberOfDiskExtents != 1) {
                printf("  Volume %s ocupa mais de um disco: lido direto, sem snapshot\n", paths[0] ? paths : vol);
                continue;
            }
            strcat(vol, "\\");
            MultiByteToWideChar(CP_ACP, 0, vol, -1, wvol, MAX_PATH);
            vol[strlen(vol) - 1] = 0;
            {
                GUID nullid;
                memset(&nullid, 0, sizeof(nullid));
                hr = VM(g_vss, g_vidx->add, vf_add)(g_vss, wvol, nullid, &snapid[n]);
            }
            if (FAILED(hr)) {
                printf("  Volume %s: sem snapshot (%s), sera lido direto\n", paths[0] ? paths : "(sem letra)",
                       (unsigned long)hr == 0x8004230C ? "FAT/sistema de arquivos nao suportado" : vss_errtext(hr));
                continue;
            }
            g_snap[n].start = (u64)e->Extents[0].StartingOffset.QuadPart;
            g_snap[n].len = (u64)e->Extents[0].ExtentLength.QuadPart;
            g_snap[n].h = NULL;
            snprintf(g_snap[n].vol, sizeof(g_snap[n].vol), "%s", vol);
            snprintf(g_snap[n].label, sizeof(g_snap[n].label), "%s", paths[0] ? paths : "(sem letra)");
            n++;
        } while (FindNextVolumeA(fv, vol, MAX_PATH));
        FindVolumeClose(fv);
    }
    if (!n) { strcpy(g_vss_note, "nenhum volume deste disco suporta snapshot"); vss_end(0); return -1; }

    hr = VM(g_vss, g_vidx->prepare, vf_async)(g_vss, &a);
    if (FAILED(hr) || FAILED(hr = vss_wait(a))) VSS_FAIL("PrepareForBackup", hr);
    printf("  Criando snapshot (pode levar ate um minuto)...\n");
    hr = VM(g_vss, g_vidx->doset, vf_async)(g_vss, &a);
    if (FAILED(hr) || FAILED(hr = vss_wait(a))) VSS_FAIL("DoSnapshotSet", hr);

    g_nsnap = n;
    for (i = 0; i < n; i++) {
        VssSnapProp sp;
        memset(&sp, 0, sizeof(sp));
        hr = VM(g_vss, g_vidx->props, vf_props)(g_vss, snapid[i], &sp);
        if (FAILED(hr) || !sp.SnapshotDeviceObject) VSS_FAIL("GetSnapshotProperties", hr);
        g_snap[i].h = CreateFileW(sp.SnapshotDeviceObject, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        CoTaskMemFree(sp.SnapshotDeviceObject);
        CoTaskMemFree(sp.OriginalVolumeName);
        CoTaskMemFree(sp.OriginatingMachine);
        CoTaskMemFree(sp.ServiceMachine);
        CoTaskMemFree(sp.ExposedName);
        CoTaskMemFree(sp.ExposedPath);
        if (g_snap[i].h == INVALID_HANDLE_VALUE) {
            g_snap[i].h = NULL;
            VSS_FAIL("abrir snapshot", (HRESULT)HRESULT_FROM_WIN32(GetLastError()));
        }
        printf("  Snapshot do volume %s criado\n", g_snap[i].label);
    }
    return 0;
}

/* No Windows 64 bits, o executavel de 32 bits passa a execucao para a versao
   64 bits (exe64, na mesma pasta), necessaria para o VSS. 0 = relancado. */
static int relaunch_64(const char *exe64, int wait, DWORD *code)
{
    WCHAR path[MAX_PATH], w64[64], *cl, *cmd, *p;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!is_wow64()) return -1;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return -1;
    p = wcsrchr(path, L'\\');
    if (!p) return -1;
    MultiByteToWideChar(CP_ACP, 0, exe64, -1, w64, 64);
    wcscpy(p + 1, w64);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return -1;
    cl = GetCommandLineW(); /* repassa os argumentos (tudo depois do nome do programa) */
    if (*cl == L'"') { cl++; while (*cl && *cl != L'"') cl++; if (*cl) cl++; }
    else while (*cl && *cl != L' ' && *cl != L'\t') cl++;
    cmd = malloc((wcslen(path) + wcslen(cl) + 4) * sizeof(WCHAR));
    if (!cmd) return -1;
    wcscpy(cmd, L"\"");
    wcscat(cmd, path);
    wcscat(cmd, L"\"");
    wcscat(cmd, cl);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { free(cmd); return -1; }
    free(cmd);
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
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

typedef struct {
    int num;
    u64 size;
    u32 sector;
    int is_system;
    char model[128];
    char letters[128];
} DiskInfo;

/* coleta informacoes do disco fisico n; retorna -1 se nao existe/inacessivel */
static int disk_info(int n, int sysd, DiskInfo *di)
{
    char spec[16];
    Dev d;
    u8 buf[1024];
    DWORD br;
    STORAGE_PROPERTY_QUERY q;
    char l;
    memset(di, 0, sizeof(*di));
    sprintf(spec, "%d", n);
    if (dev_open(&d, spec, 0) != 0) return -1;
    di->num = n;
    di->size = d.size;
    di->sector = d.sector;
    di->is_system = (n == sysd);
    memset(&q, 0, sizeof(q));
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    if (DeviceIoControl(d.h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), buf, sizeof(buf), &br, NULL)) {
        STORAGE_DEVICE_DESCRIPTOR *sd = (STORAGE_DEVICE_DESCRIPTOR *)buf;
        if (sd->VendorIdOffset && sd->VendorIdOffset < br) {
            strncat(di->model, (char *)buf + sd->VendorIdOffset, 40);
            strcat(di->model, " ");
        }
        if (sd->ProductIdOffset && sd->ProductIdOffset < br)
            strncat(di->model, (char *)buf + sd->ProductIdOffset, 60);
    }
    /* remove espacos extras */
    {
        char *p = di->model, *o = di->model;
        int sp = 1;
        for (; *p; p++) {
            if (*p == ' ') { if (!sp) *o++ = ' '; sp = 1; }
            else { *o++ = *p; sp = 0; }
        }
        if (o > di->model && o[-1] == ' ') o--;
        *o = 0;
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
            strcat(di->letters, t);
        }
        CloseHandle(h);
    }
    dev_close(&d);
    return 0;
}

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_list(void)
{
    int n, found = 0, sysd = system_disk();
    printf("\nDiscos fisicos encontrados:\n\n");
    printf("  No.  Tamanho      Modelo / Volumes\n");
    printf("  ---  -----------  ------------------------------------------\n");
    for (n = 0; n < 32; n++) {
        DiskInfo di;
        char sz[32];
        if (disk_info(n, sysd, &di) != 0) continue;
        found++;
        fmt_size(di.size, sz);
        printf("  %-3d  %-11s  %s\n", n, sz, di.model[0] ? di.model : "(desconhecido)");
        printf("                    volumes: %s%s  setor: %u bytes\n",
               di.letters[0] ? di.letters : "(nenhum)", di.is_system ? " <- DISCO DO WINDOWS EM USO" : "",
               di.sector);
    }
    if (!found)
        printf("  Nenhum disco acessivel. Execute como Administrador.\n");
    printf("\n");
}
#else
static int lock_disk_volumes(int disknum, int dismount) { (void)disknum; (void)dismount; return 0; }
static void unlock_all(void) {}
static int path_disk(const char *p) { (void)p; return -1; }
/* Linux: simula snapshots para testar o redirecionamento de leitura.
   HDCLONE_TEST_SNAP="inicio_bytes:tamanho_bytes:arquivo[,...]" */
static int vss_begin(int disknum)
{
    const char *e = getenv("HDCLONE_TEST_SNAP");
    (void)disknum;
    g_nsnap = 0;
    while (e && *e && g_nsnap < MAX_SNAPS) {
        char f[512];
        unsigned long long st, ln;
        int used = 0;
        if (sscanf(e, "%llu:%llu:%511[^,]%n", &st, &ln, f, &used) != 3) break;
        g_snap[g_nsnap].start = st;
        g_snap[g_nsnap].len = ln;
        g_snap[g_nsnap].h = open(f, O_RDONLY);
        if (g_snap[g_nsnap].h < 0) return -1;
        printf("  [teste] snapshot simulado em %llu (+%llu): %s\n", st, ln, f);
        g_nsnap++;
        e += used;
        if (*e == ',') e++;
    }
    return g_nsnap ? 0 : -1;
}
static void vss_end(int ok)
{
    int i;
    (void)ok;
    for (i = 0; i < g_nsnap; i++) close(g_snap[i].h);
    g_nsnap = 0;
}
static char g_vss_note[300];
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

static void reset_areas(void)
{
    int i;
    for (i = 0; i < g_nareas; i++) free(g_areas[i].bm);
    g_nareas = 0;
}

#ifdef HDCLONE_GUI
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
#endif

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

/* desfaz a protecao "update sequence" de um registro da MFT */
static int ntfs_fixup(u8 *rec, u32 recsz)
{
    u32 usa_off = le16(rec + 4), usa_cnt = le16(rec + 6), i;
    if (memcmp(rec, "FILE", 4) != 0 || usa_off + 2 * usa_cnt > recsz) return -1;
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
    add_area(off, g.cs, g.ncl, bm);
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
#ifdef HDCLONE_GUI
    (void)pct;
    ui_progress(done, total, speed, eta);
    return;
#endif
    printf("\r  %5.1f%%  %llu / %llu MB  %6.1f MB/s  restante %02lu:%02lu:%02lu ",
           pct, (unsigned long long)(done >> 20), (unsigned long long)(total >> 20), speed,
           eta / 3600, (eta / 60) % 60, eta % 60);
    fflush(stdout);
}

static int confirm(const char *msg)
{
    char line[64];
#ifdef HDCLONE_GUI
    (void)msg; (void)line;
    return 1; /* a interface grafica ja confirmou antes de iniciar */
#endif
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
    int no_vss;
} Opts;

static int g_vss_used; /* ultimo backup usou VSS */
static int g_shrunk;   /* ultima restauracao reduziu uma particao */

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

    g_vss_used = 0;
#ifdef _WIN32
    if (d.disknum >= 0 && !o->no_vss) {
#else
    if (getenv("HDCLONE_TEST_SNAP") && !o->no_vss) {
#endif
        printf("\nVSS (copia consistente com o Windows ligado):\n");
        if (vss_begin(d.disknum) == 0) {
            d.use_snap = 1;
            g_vss_used = 1;
        } else {
            printf("  AVISO: VSS indisponivel - %s\n", g_vss_note);
        }
    }

    if (!o->raw) {
        printf("\nAnalisando particoes...\n");
        reset_areas();
        scan_partitions(&d);
    } else {
        printf("\nModo setor a setor (--raw): todo o disco sera copiado\n");
    }

    if (d.disknum >= 0) {
        printf("\nBloqueando volumes sem snapshot...\n");
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
    put32(hdr + 20, (o->raw ? 0 : 1) | (g_vss_used ? 2 : 0));
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

        if (g_cancel) die("Operacao cancelada pelo usuario.");

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
    vss_end(1);
    dev_close(&d);

    fmt_size(d.size, s1);
    fmt_size(m.total, s2);
    printf("\n\nBackup concluido!\n");
    printf("  Disco: %s   Imagem: %s (%.1f%% do disco)   Partes: %d\n", s1, s2,
           100.0 * (double)m.total / (double)d.size, m.part);
    if (nfree) printf("  Blocos de espaco livre ignorados: %llu MB\n", (unsigned long long)nfree);
    if (bad) printf("  ATENCAO: %llu setores ilegiveis foram preenchidos com zeros!\n", (unsigned long long)bad);
    if (g_vss_used) printf("  Copia consistente feita via VSS (snapshots temporarios ja removidos).\n");
    xfree(buf);
    xfree(cbuf);
    reset_areas();
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
    printf("  Modo           : %s%s\n", (h->flags & 1) ? "inteligente (somente dados usados)" : "setor a setor",
           (h->flags & 2) ? ", copia consistente via VSS" : "");
}

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

typedef struct {
    Img m;
    Hdr h;
    ChunkIdx *ix;
    u8 *cache, *cbuf;
    i64 ci;
} ImgSrc;

static ImgSrc g_is;

/* percorre a imagem lendo so os cabecalhos dos blocos e monta um indice */
static void imgsrc_open(const char *path)
{
    ImgSrc *s = &g_is;
    u64 i;
    memset(s, 0, sizeof(*s));
    if (img_open(&s->m, path, 0, 0) != 0) die("nao foi possivel abrir a imagem");
    read_header(&s->m, &s->h);
    s->ix = xalloc((size_t)s->h.nchunks * sizeof(ChunkIdx));
    g_t0 = g_tlast = now_ms();
    for (i = 0; i < s->h.nchunks; i++) {
        u8 ch[9];
        ChunkIdx *c = &s->ix[i];
        if (g_cancel) die("Operacao cancelada pelo usuario.");
        if (img_read(&s->m, ch, 9) != 0) die("imagem incompleta (falta alguma parte?)");
        c->type = ch[0];
        c->stored = le32(ch + 1);
        c->crc = le32(ch + 5);
        if (c->type > CH_FREE || c->stored > compressBound(CHUNK_SIZE)) die("imagem corrompida");
        if (c->type == CH_DEFLATE || c->type == CH_RAW) {
            c->part = s->m.part;
            c->pos = s->m.pos;
            if (img_skip(&s->m, c->stored) != 0) die("imagem incompleta (falta alguma parte?)");
        }
        progress((i + 1) * CHUNK_SIZE < s->h.size ? (i + 1) * CHUNK_SIZE : s->h.size, s->h.size,
                 i + 1 == s->h.nchunks);
    }
    s->cache = xalloc(CHUNK_SIZE);
    s->cbuf = xalloc(compressBound(CHUNK_SIZE));
    s->ci = -1;
}

static void imgsrc_close(void)
{
    ImgSrc *s = &g_is;
    img_close_part(&s->m);
    xfree(s->ix);
    xfree(s->cache);
    xfree(s->cbuf);
    memset(s, 0, sizeof(*s));
}

static int imgsrc_chunk(ImgSrc *s, u64 i)
{
    ChunkIdx *c = &s->ix[i];
    u64 off = i * CHUNK_SIZE;
    u32 len = (u32)((s->h.size - off) < CHUNK_SIZE ? (s->h.size - off) : CHUNK_SIZE);
    if (s->ci == (i64)i) return 0;
    s->ci = -1;
    if (c->type == CH_ZERO || c->type == CH_FREE) {
        memset(s->cache, 0, len);
    } else {
        if (img_seek(&s->m, c->part, c->pos) != 0) return -1;
        if (c->type == CH_RAW) {
            if (img_read(&s->m, s->cache, len) != 0) return -1;
        } else {
            uLongf ul = CHUNK_SIZE;
            if (img_read(&s->m, s->cbuf, c->stored) != 0) return -1;
            if (uncompress(s->cache, &ul, s->cbuf, c->stored) != Z_OK || ul != len) return -1;
        }
        if ((u32)crc32(0, s->cache, len) != c->crc) return -1;
    }
    s->ci = (i64)i;
    return 0;
}

static int imgsrc_pread(void *src, void *buf, u32 len, u64 off)
{
    ImgSrc *s = src;
    u8 *p = buf;
    while (len) {
        u64 i = off / CHUNK_SIZE;
        u32 o = (u32)(off % CHUNK_SIZE), n = CHUNK_SIZE - o;
        if (n > len) n = len;
        if (off + n > s->h.size || imgsrc_chunk(s, i) != 0) return -1;
        memcpy(p, s->cache + o, n);
        p += n; off += n; len -= n;
    }
    return 0;
}

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
                     "faca o backup de novo; isso junta os dados no inicio da particao.", s1, s2);
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

/* processa a imagem; se d != NULL grava no disco */
static void process_image(const char *path, Dev *d, Opts *o, Hdr *h, Img *m, u64 limit)
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
        if (g_cancel) die("Operacao cancelada pelo usuario.");
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
        if (d && data && off < limit && dev_pwrite(d, data, off + len > limit ? (u32)(limit - off) : len, off) != 0) {
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

#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
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
    process_image(path, NULL, &o, &h, &m, 0);
    img_close_part(&m);
    printf("\n\nImagem OK: todos os blocos conferem (CRC32).\n");
}

static void cmd_restore(const char *path, const char *dst, Opts *o)
{
    Img m;
    Hdr h;
    Dev d;
    Plan *pl = NULL;
    u64 T;
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

    /* tamanho efetivo do destino. Arquivo: cresce ate o tamanho original, a menos
       que ja exista com tamanho menor (simula um disco menor) */
    T = d.size;
    if (!d.is_device && (d.size == 0 || d.size >= h.size)) T = h.size;
    if (T < h.size) {
        Dev v;
        static Plan plan;
        printf("\nDestino menor que o original (%s < %s). Analisando a imagem...\n", s2, s1);
        imgsrc_open(path);
        memset(&v, 0, sizeof(v));
        v.src = &g_is;
        v.size = h.size;
        v.sector = h.sector;
        v.disknum = -1;
        plan_shrink(&v, T, &plan); /* aborta aqui (sem gravar nada) se nao couber */
        imgsrc_close();
        pl = &plan;
    }
    if (d.is_device) {
        if (d.sector != h.sector) {
            printf("\nAVISO: tamanho de setor diferente (origem %u, destino %u). As particoes\n"
                   "provavelmente NAO funcionarao no destino.\n", h.sector, d.sector);
            if (!o->force) die("use --force para restaurar mesmo assim");
        }
    }

    if (!o->yes) {
        sprintf(msg, "\nATENCAO: TODO o conteudo de %s sera APAGADO e substituido pela imagem!", d.name);
        if (!confirm(msg)) die("Cancelado.");
    }

    if (d.disknum >= 0) {
        printf("\nDesmontando volumes do destino...\n");
        if (lock_disk_volumes(d.disknum, 1) > 0 && !o->force)
            die("ha volumes em uso no disco de destino (feche programas/janelas ou use --force)");
    }

    printf("\nRestaurando...\n");
    process_image(path, &d, o, &h, &m, T);
    img_close_part(&m);

    if (!d.is_device) {
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)T;
        if (d.size < T) { SetFilePointerEx(d.h, li, NULL, FILE_BEGIN); SetEndOfFile(d.h); }
#else
        if (d.size < T && ftruncate(d.fd, (off_t)T) != 0) die("falha ao ajustar tamanho do arquivo");
#endif
        d.size = d.size > T ? d.size : T;
    }
    printf("\n\nFinalizando...\n");
    if (pl) apply_shrink(&d, pl);
    if (d.size != h.size) fix_gpt(&d);
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
    g_shrunk = pl && pl->has_part;
    if (pl && pl->has_part)
        printf("A particao foi reduzida para caber no disco. No primeiro boot o Windows pode\n"
               "rodar uma verificacao de disco (chkdsk): deixe terminar, e normal.\n");
    if (d.size > h.size)
        printf("Obs.: o disco de destino e maior. O espaco extra ficou nao alocado; use o\n"
               "Gerenciamento de Disco (Estender volume) ou diskpart para aproveita-lo.\n");
}

/* ------------------------------------------------------------------ */
/* interface                                                           */
/* ------------------------------------------------------------------ */

#ifndef HDCLONE_GUI
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
        "  --no-vss     nao usar VSS (copia de sombra). Por padrao o VSS e usado para\n"
        "               copiar o disco com o Windows ligado de forma consistente.\n"
        "Opcoes de restore:\n"
        "  -y           nao pedir confirmacao\n"
        "  --zero-free  grava zeros no espaco livre (mais lento, apaga dados antigos)\n"
        "  --force      ignora avisos (volumes em uso, setor diferente)\n\n"
        "Exemplos:\n"
        "  hdclone backup 1 E:\\backup\\pc-sala.hdc\n"
        "  hdclone restore E:\\backup\\pc-sala.hdc 2\n\n"
        "Execute como Administrador. O disco do Windows em uso e copiado de forma\n"
        "consistente via VSS (Windows 64 bits: deixe o hdclone64.exe na mesma pasta).\n");
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
#ifdef _WIN32
    {
        DWORD code = 1;
        if (relaunch_64("hdclone64.exe", 1, &code) == 0) return (int)code;
    }
#endif

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
        else if (!strcmp(argv[i], "--no-vss")) o.no_vss = 1;
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
#endif /* HDCLONE_GUI */
