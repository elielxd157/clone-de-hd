/* hdclone - tipos, utilitarios, registro (.log), memoria */

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
#include <stdarg.h>
#include <zlib.h>
#include "../zstd/zstd.h"

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
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <pthread.h>
#include <semaphore.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#endif

#define VERSION_STR "hdclone 2.0"
#define CHUNK_SIZE (1024u * 1024u)
#define HDR_SIZE 512
#define FTR_SIZE 48
#define CH_ZERO 0
#define CH_DEFLATE 1
#define CH_RAW 2
#define CH_FREE 3
#define CH_ZSTD 4   /* comprimido com zstd (imagens versao 2) */

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
#endif

/* ---- registro em arquivo (.log ao lado da imagem) ---- */

static FILE *g_logf;
static time_t g_log_t0;

static void log_text(const char *s)
{
    if (!g_logf || s[0] == '\r') return; /* linhas de progresso nao vao para o arquivo */
    fputs(s, g_logf);
    fflush(g_logf);
}

/* toda a saida do programa passa por aqui: tela (ou janela) + arquivo de log */
static void hd_printf(const char *fmt, ...)
{
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    log_text(b);
#ifdef HDCLONE_GUI
    ui_log("%s", b);
#else
    fputs(b, stdout);
#endif
}
#define printf hd_printf

static void log_open(const char *path, const char *what, int append)
{
    char t[64];
    struct tm *tm;
    g_log_t0 = time(NULL);
    g_logf = fopen(path, append ? "a" : "w");
    if (!g_logf) return;
    tm = localtime(&g_log_t0);
    if (tm) strftime(t, sizeof(t), "%d/%m/%Y %H:%M:%S", tm); else strcpy(t, "?");
    fprintf(g_logf, "hdclone 2.0 - %s - inicio: %s\n", what, t);
    fflush(g_logf);
}

static void log_close(const char *result)
{
    char t[64];
    time_t now = time(NULL);
    struct tm *tm;
    long el = (long)(now - g_log_t0);
    if (!g_logf) return;
    tm = localtime(&now);
    if (tm) strftime(t, sizeof(t), "%d/%m/%Y %H:%M:%S", tm); else strcpy(t, "?");
    fprintf(g_logf, "\n\nResultado: %s\nFim: %s (duracao %02ld:%02ld:%02ld)\n", result, t, el / 3600, (el / 60) % 60,
            el % 60);
    fclose(g_logf);
    g_logf = NULL;
}

static void die(const char *msg)
{
    if (g_logf) {
        char b[700];
        snprintf(b, sizeof(b), "\nERRO: %s\n", msg);
        log_text(b);
        log_close("FALHOU");
    }
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
