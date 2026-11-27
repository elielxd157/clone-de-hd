/* Testa o caminho usado pela interface grafica: erros e cancelamento encerram so a
   operacao (sem fechar o programa) e liberam todos os recursos. */
/* testa o caminho da GUI (erros nao fatais, cancelamento, limpeza) no Linux */
#define HDCLONE_GUI
#include "hdclone.c"
#include <setjmp.h>
#include <stdarg.h>
static jmp_buf jb; static int cancel_at = -1, nprog;
static void ui_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); if (cancel_at == 3) { const char *x = strcmp(fmt, "%s") ? fmt : va_arg(ap, const char *); if (strstr(x, "Copiando") || strstr(x, "Clonando")) g_cancel = 1; } va_end(ap); }
static void ui_progress(u64 d, u64 t, double s, unsigned long e) { (void)d;(void)t;(void)s;(void)e; if (++nprog == cancel_at) g_cancel = 1; }
static char err[600];
static void ui_fatal(const char *m) { strcpy(err, m); cleanup_all(); longjmp(jb, 1); }
static int live(void) { int i, n = 0; for (i = 0; i < MAX_RES; i++) if (g_res[i].p) n++; return n; }
static int run(int mode, const char *a, const char *b) {
  Opts o; memset(&o,0,sizeof o); o.level=6; o.yes=1; err[0] = 0; g_cancel = 0; nprog = 0;
  if (setjmp(jb) == 0) { if (mode == 0) cmd_backup(a, b, &o); else if (mode == 1) cmd_restore(a, b, &o); else if (mode == 3) cmd_clone(a, b, &o); else cmd_verify(a); return 1; }
  return 0;
}
#undef printf
int main(int c, char **v) {
  int r; (void)c;
  r = run(0, "/nao/existe", "/tmp/x.hdc"); printf("1 erro origem: ok=%d err='%s' recursos=%d\n", r, err, live());
  cancel_at = 3; r = run(0, v[1], v[2]); printf("2 cancelado:   ok=%d err='%s' recursos=%d areas=%d\n", r, err, live(), g_nareas);
  cancel_at = -1; r = run(0, v[1], v[2]); printf("3 backup:     ok=%d recursos=%d areas=%d\n", r, live(), g_nareas);
  r = run(2, v[2], 0); printf("4 verify:     ok=%d recursos=%d\n", r, live());
  r = run(1, v[2], v[3]); printf("5 restore:    ok=%d recursos=%d\n", r, live());
  r = run(3, v[1], v[3]); printf("5b clone:     ok=%d recursos=%d areas=%d\n", r, live(), g_nareas);
  cancel_at = 3; r = run(3, v[1], v[3]); printf("5c clone cancelado: ok=%d err='%s' recursos=%d\n", r, err, live()); cancel_at = -1;
  printf("   log ainda aberto? %s\n", g_logf ? "SIM (erro)" : "nao");
  FILE *f = fopen(v[2], "r+b"); fseek(f, 300000, 0); fputs("LIXO", f); fclose(f);
  r = run(2, v[2], 0); printf("6 corrompida: ok=%d err='%s' recursos=%d\n", r, err, live());
  return 0;
}
