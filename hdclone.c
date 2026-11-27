/*
 * hdclone - clonagem de disco para imagem compactada (.hdc), restauracao,
 * clone disco a disco e recuperacao de arquivos. Windows XP ate 11 (e Linux, para testes).
 *
 * O codigo fica em src/, dividido por assunto. Ele e compilado como uma unica
 * unidade (os arquivos sao incluidos em ordem), por isso as funcoes continuam
 * "static" e o executavel gerado e o mesmo de antes da divisao.
 */

#include "src/base.c"
#include "src/dispositivo.c"
#include "src/imagem_arquivo.c"
#include "src/windows_vss.c"
#include "src/mapa_ntfs_fat.c"
#include "src/leitor_fs.c"
#include "src/backup.c"
#include "src/imagem_virtual.c"
#include "src/disco_menor.c"
#include "src/explorar.c"
#include "src/hwfix_restore.c"
#include "src/clone.c"
#include "src/main.c"
