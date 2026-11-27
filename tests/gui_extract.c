/* Testa cancelar uma extracao e extrair de novo com a mesma imagem aberta. */
#define HDCLONE_GUI
#include "hdclone.c"
#include <setjmp.h>
#include <stdarg.h>
static int nprog;
static void ui_log(const char *fmt, ...) { (void)fmt; }
static void ui_progress(u64 d, u64 t, double s, unsigned long e) { (void)d;(void)t;(void)s;(void)e; if (++nprog == 1) g_cancel = 1; }
static void ui_fatal(const char *m) { fprintf(stderr, "FATAL %s\n", m); exit(2); }
#undef printf
int main(int c, char **v) {
  VolInfo vol[8]; Fs nt; DirList dl; int i, n; (void)c;
  open_virtual(v[1]); n = image_volumes(&g_vdev, vol, 8); fs_open(&nt, &g_vdev, vol[0].start, vol[0].len); fs_list(&nt, fs_root(&nt), &dl);
  for (i = 0; i < dl.n && !dl.e[i].is_dir; i++); /* primeira pasta */
  g_tlast = 0; g_t0 = 0;  /* forca progresso imediato */
  extract_entries(&nt, dl.e, dl.n, v[2]);
  printf("1a extracao: cancelada=%d arquivos=%llu\n", g_xs.cancelled, (unsigned long long)g_xs.files);
  g_cancel = 0; extract_entries(&nt, dl.e, dl.n, v[2]);
  printf("2a extracao (mesma imagem aberta): cancelada=%d arquivos=%llu erros=%llu\n", g_xs.cancelled, (unsigned long long)g_xs.files, (unsigned long long)g_xs.errors);
  return 0;
}
