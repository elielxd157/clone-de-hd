/* hdclone - linha de comando */

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
        "  hdclone clone   <origem> <destino>   copia um disco direto para outro\n"
        "  hdclone verify  <img.hdc>            confere a integridade da imagem\n"
        "  hdclone info    <img.hdc>            mostra informacoes da imagem\n"
        "  hdclone ls      <img.hdc> [N [pasta]]           lista particoes / pastas da imagem\n"
        "  hdclone extract <img.hdc> N <caminho> <destino> copia arquivo/pasta da imagem\n"
        "  hdclone find    <img.hdc> N <nome> [pasta]      procura arquivos (aceita * e ?)\n"
        "  hdclone hwfix   <disco>              prepara o Windows de um disco para outro PC\n"
        "  hdclone selftest                     autoteste (nao altera nenhum disco)\n\n"
        "<disco> = numero mostrado em 'list' (ex.: 1) ou caminho de arquivo/dispositivo.\n\n"
        "Opcoes de backup:\n"
        "  -c N         nivel de compressao zstd: 1 (rapido) a 19 (menor). Padrao: 3\n"
        "               (1 = rapida, 3 = normal, 9 = maxima)\n"
        "  --compat     formato antigo (zlib, niveis 1 a 9), legivel por versoes\n"
        "               anteriores do hdclone. Mais lento.\n"
        "  -s MB        dividir a imagem em partes de MB megabytes. Padrao: 4000\n"
        "               (cabe em pendrive FAT32). Use -s 0 para arquivo unico.\n"
        "  --raw        copia setor a setor, inclusive espaco livre\n"
        "  --verify     backup/clone: ao terminar, le tudo de volta e confere\n"
        "               restore: confere a imagem inteira ANTES de apagar o disco de destino\n"
        "  --resume     continua um backup interrompido (cabo solto, falta de energia...)\n"
        "  --skip-restore-points  deixa de fora (grava como zeros) os pontos de restauracao\n"
        "               do Windows Vista+ e os despejos de memoria (MEMORY.DMP etc.)\n"
        "  --rescue-all disco com setores defeituosos: tenta ler todos, um a um (bem mais\n"
        "               lento; o padrao pula rapido as areas ruins)\n"
        "  --keep-sys   copiar tambem pagefile.sys/hiberfil.sys/swapfile.sys (por padrao\n"
        "               ficam de fora: o Windows recria esses arquivos)\n"
        "  --no-vss     nao usar VSS (copia de sombra). Por padrao o VSS e usado para\n"
        "               copiar o disco com o Windows ligado de forma consistente.\n"
        "Opcoes de restore:\n"
        "  -y           nao pedir confirmacao\n"
        "  --zero-free  grava zeros no espaco livre (mais lento, apaga dados antigos)\n"
        "  --hwfix      prepara o Windows para outro hardware (evita tela azul 0x7B)\n"
        "  --force      ignora avisos (volumes em uso, setor diferente)\n\n"
        "Exemplos:\n"
        "  hdclone backup 1 E:\\backup\\pc-sala.hdc\n"
        "  hdclone restore E:\\backup\\pc-sala.hdc 2\n"
        "  hdclone extract E:\\backup\\pc-sala.hdc 1 \"Users\\Ana\\Documents\" D:\\recuperado\n\n"
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
    char *pos[6];
    int np = 0, i;
    memset(&o, 0, sizeof(o));
    o.split = 4000ull * 1024 * 1024;
#ifdef _WIN32
    {
        DWORD code = 1;
        if (relaunch_64("hdclone64.exe", 1, &code) == 0) return (int)code;
    }
#endif

    if (argc < 2) {
        o.level = 3;
        interactive(&o);
        return 0;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            o.level = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--compat")) {
            o.compat = 1;
        } else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
            const char *q = argv[++i];
            u64 v = 0;
            while (*q >= '0' && *q <= '9') v = v * 10 + (u64)(*q++ - '0');
            o.split = v * 1024 * 1024;
        } else if (!strcmp(argv[i], "--raw")) o.raw = 1;
        else if (!strcmp(argv[i], "-y")) o.yes = 1;
        else if (!strcmp(argv[i], "--force")) o.force = 1;
        else if (!strcmp(argv[i], "--zero-free")) o.zero_free = 1;
        else if (!strcmp(argv[i], "--verify")) o.verify = 1;
        else if (!strcmp(argv[i], "--rescue-all")) o.rescue_all = 1;
        else if (!strcmp(argv[i], "--skip-restore-points")) o.skip_restore = 1;
        else if (!strcmp(argv[i], "--resume")) o.resume = 1;
        else if (!strcmp(argv[i], "--hwfix")) o.hwfix = 1;
        else if (!strcmp(argv[i], "--no-vss")) o.no_vss = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || !strcmp(argv[i], "/?")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--keep-sys")) o.keep_sys = 1;
        else if (np < 6) pos[np++] = argv[i];
        else { usage(); return 1; }
    }
    if (!o.level) o.level = o.compat ? 6 : 3;
    if (o.compat ? (o.level < 1 || o.level > 9) : (o.level < 1 || o.level > 19))
        die(o.compat ? "com --compat o nivel de compressao vai de 1 a 9" : "nivel de compressao deve ser de 1 a 19");
    if (np == 1 && !strcmp(pos[0], "list")) cmd_list();
    else if (np == 3 && !strcmp(pos[0], "backup")) cmd_backup(pos[1], pos[2], &o);
    else if (np == 3 && !strcmp(pos[0], "restore")) cmd_restore(pos[1], pos[2], &o);
    else if (np == 2 && !strcmp(pos[0], "verify")) cmd_verify(pos[1]);
    else if (np == 2 && !strcmp(pos[0], "info")) cmd_info(pos[1]);
    else if (np >= 2 && np <= 4 && !strcmp(pos[0], "ls")) cmd_ls(pos[1], np > 2 ? pos[2] : NULL, np > 3 ? pos[3] : NULL);
    else if (np == 5 && !strcmp(pos[0], "extract")) cmd_extract(pos[1], pos[2], pos[3], pos[4]);
    else if (np == 2 && !strcmp(pos[0], "hwfix")) cmd_hwfix(pos[1], &o);
    else if (np == 1 && !strcmp(pos[0], "selftest")) { cmd_selftest(); return g_st.fail ? 1 : 0; }
    else if (np == 3 && !strcmp(pos[0], "clone")) cmd_clone(pos[1], pos[2], &o);
    else if ((np == 4 || np == 5) && !strcmp(pos[0], "find")) cmd_find(pos[1], pos[2], pos[3], np == 5 ? pos[4] : NULL);
    else { usage(); return 1; }
    return 0;
}
#endif /* HDCLONE_GUI */
