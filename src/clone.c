/* hdclone - clone disco a disco */

/* ------------------------------------------------------------------ */
/* CLONE: disco -> disco, sem imagem intermediaria                     */
/* ------------------------------------------------------------------ */

typedef struct {
    BkCtx bk;   /* primeiro campo: bk_produce le o disco de origem com o mesmo codigo do backup */
    Dev *t;
    u64 limit;
    u8 *zeros;
    u32 *crc;   /* CRC de cada bloco gravado inteiro (para a verificacao) */
    u8 *wrote;
} ClCtx;

static int cl_work(Pipe *p, Slot *sl, int w)
{
    (void)p;
    (void)w;
    if (sl->type == T_PENDING) sl->crc = (u32)crc32(0, sl->buf, sl->len);
    return 0;
}

static int cl_consume(Pipe *p, Slot *sl)
{
    ClCtx *c = p->ctx;
    u64 off = sl->idx * CHUNK_SIZE;
    const u8 *data = NULL;
    if (sl->type == T_PENDING) data = sl->buf;
    else if (sl->type == CH_ZERO || (sl->type == CH_FREE && c->bk.o->zero_free)) data = c->zeros;
    if (data && off < c->limit) {
        u32 n = off + sl->len > c->limit ? (u32)(c->limit - off) : sl->len;
        if (dev_pwrite(c->t, data, n, off) != 0) {
            char msg[200];
            snprintf(msg, sizeof(msg), "falha de escrita no disco de destino na posicao %llu MB",
                     (unsigned long long)(off >> 20));
            pipe_fail(p, msg);
            return -1;
        }
        if (n == sl->len) {
            c->crc[sl->idx] = data == c->zeros ? (u32)crc32(0, c->zeros, n) : sl->crc;
            c->wrote[sl->idx] = 1;
        }
    }
    if (sl->type == T_PENDING) c->bk.used += sl->len;
    p->done_bytes += sl->len;
    return 0;
}

static void cmd_clone(const char *src, const char *dst, Opts *o)
{
    Dev sd, td;
    Plan *pl = NULL;
    static Plan plan;
    u64 T, nchunks, i;
    char s1[32], s2[32], msg[1400];
    Pipe pp;
    ClCtx cc;
    int rc;

    if (dev_open(&sd, src, 0) != 0) die("nao foi possivel abrir o disco de origem (execute como Administrador)");
    if (dev_open(&td, dst, 1) != 0) die("nao foi possivel abrir o disco de destino (execute como Administrador)");
    if ((sd.disknum >= 0 && sd.disknum == td.disknum) || !strcmp(sd.name, td.name))
        die("origem e destino sao o mesmo disco");
#ifdef _WIN32
    if (td.disknum >= 0 && td.disknum == system_disk()) die("o disco de destino e o disco do Windows em uso");
#endif
    if (!sd.size) die("nao foi possivel obter o tamanho do disco de origem");
    T = td.size;
    if (!td.is_device && (td.size == 0 || td.size >= sd.size)) T = sd.size;
    fmt_size(sd.size, s1);
    fmt_size(T, s2);
    printf("\nOrigem : %s  (%s, setor %u bytes)\n", sd.name, s1, sd.sector);
    printf("Destino: %s  (%s)\n", td.name, s2);
    if (td.is_device && td.sector != sd.sector) {
        printf("\nAVISO: tamanho de setor diferente (origem %u, destino %u). As particoes\n"
               "provavelmente NAO funcionarao no destino.\n", sd.sector, td.sector);
        if (!o->force) die("use --force para clonar mesmo assim");
    }

    g_vss_used = 0;
#ifdef _WIN32
    if (sd.disknum >= 0 && !o->no_vss) {
#else
    if (getenv("HDCLONE_TEST_SNAP") && !o->no_vss) {
#endif
        printf("\nVSS (copia consistente com o Windows ligado):\n");
        if (vss_begin(sd.disknum) == 0) {
            sd.use_snap = 1;
            g_vss_used = 1;
        } else {
            printf("  AVISO: VSS indisponivel - %s\n", g_vss_note);
        }
    }
    if (!o->raw) {
        printf("\nAnalisando particoes da origem...\n");
        g_keep_sys = o->keep_sys;
        g_skip_restore = o->skip_restore;
        reset_areas();
        scan_partitions(&sd);
    }
    if (T < sd.size) {
        printf("\nDestino menor que a origem. Conferindo se os dados cabem...\n");
        plan_shrink(&sd, T, &plan); /* aborta aqui, sem gravar nada, se nao couber */
        pl = &plan;
    }
    if (!o->yes) {
        snprintf(msg, sizeof(msg), "\nATENCAO: TODO o conteudo de %s sera APAGADO e substituido pela copia de %s!",
                 td.name, sd.name);
        if (!confirm(msg)) die("Cancelado.");
    }
    if (td.disknum >= 0) {
        printf("\nDesmontando volumes do destino...\n");
        if (lock_disk_volumes(td.disknum, 1) > 0 && !o->force)
            die("ha volumes em uso no disco de destino (feche programas/janelas ou use --force)");
    }
    if (sd.disknum >= 0) {
        printf("\nBloqueando volumes da origem sem snapshot...\n");
        if (lock_disk_volumes(sd.disknum, 0) > 0 && !g_vss_used)
            printf("  AVISO: ha volumes em uso. Arquivos abertos podem ficar inconsistentes na copia.\n");
    }

    nchunks = (sd.size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    memset(&cc, 0, sizeof(cc));
    cc.bk.d = &sd;
    cc.bk.o = o;
    cc.t = &td;
    cc.limit = T;
    cc.zeros = xalloc(CHUNK_SIZE);
    cc.crc = xalloc((size_t)nchunks * 4);
    cc.wrote = xalloc((size_t)nchunks);
    pipe_init(&pp, nchunks, sd.size);
    pp.produce = bk_produce;
    pp.work = cl_work;
    pp.consume = cl_consume;
    pp.ctx = &cc;
    printf("\nClonando...\n");
    rc = pipe_run(&pp);
    pipe_free(&pp);
    if (rc != 0) die(pp.err);
    fmt_size(cc.bk.used, s1);
    printf("\n  Dados copiados: %s", s1);
    if (cc.bk.nfree) printf("   espaco livre ignorado: %llu MB", (unsigned long long)cc.bk.nfree);
    printf("\n");
    if (cc.bk.bad) printf("  ATENCAO: %llu setores ilegiveis na origem foram gravados como zero!\n",
                          (unsigned long long)cc.bk.bad);

    if (o->verify) { /* antes dos ajustes finais, que alteram alguns setores de proposito */
        u8 *buf = xalloc(CHUNK_SIZE);
        printf("\nVerificando o disco de destino (lendo de volta)...\n");
        g_t0 = g_tlast = now_ms();
        for (i = 0; i < nchunks; i++) {
            u64 off = i * CHUNK_SIZE;
            u32 len = (u32)((sd.size - off) < CHUNK_SIZE ? (sd.size - off) : CHUNK_SIZE);
            if (g_cancel) die("Operacao cancelada pelo usuario.");
            if (cc.wrote[i] && (dev_pread(&td, buf, len, off) != 0 || (u32)crc32(0, buf, len) != cc.crc[i])) {
                snprintf(msg, sizeof(msg), "o disco de destino NAO confere na posicao %llu MB (disco com defeito?)",
                         (unsigned long long)(off >> 20));
                die(msg);
            }
            progress(off + len, sd.size, i + 1 == nchunks);
        }
        xfree(buf);
        printf("\n  Disco de destino conferido: todos os blocos gravados estao corretos.\n");
    }

    if (!td.is_device) {
#ifdef _WIN32
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)T;
        if (td.size < T) { SetFilePointerEx(td.h, li, NULL, FILE_BEGIN); SetEndOfFile(td.h); }
#else
        if (td.size < T && ftruncate(td.fd, (off_t)T) != 0) die("falha ao ajustar tamanho do arquivo");
#endif
        td.size = td.size > T ? td.size : T;
    }
    printf("\nFinalizando...\n");
    if (pl) apply_shrink(&td, pl);
    if (td.size != sd.size) fix_gpt(&td);
    if (o->hwfix) {
        printf("\nPreparando o Windows para funcionar em outro hardware...\n");
        hwfix_disk(&td);
    }
#ifdef _WIN32
    if (td.is_device) {
        DWORD br;
        FlushFileBuffers(td.h);
        DeviceIoControl(td.h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);
    }
#endif
    unlock_all();
    vss_end(1);
    dev_close(&sd);
    dev_close(&td);
    xfree(cc.zeros);
    xfree(cc.crc);
    xfree(cc.wrote);
    reset_areas();
    g_shrunk = pl && pl->has_part;
    printf("\nClone concluido!\n");
    if (g_vss_used) printf("Copia consistente feita via VSS.\n");
    if (g_shrunk) printf("A ultima particao foi reduzida para caber no destino (chkdsk no primeiro boot e normal).\n");
    if (td.size > sd.size) printf("O destino e maior: o espaco extra ficou nao alocado (use Estender volume).\n");
    printf("IMPORTANTE: antes de dar boot pelo disco novo, desligue o PC e remova (ou desconecte) o\n"
           "disco antigo. Com os dois ligados, o Windows pode confundir os discos, que sao identicos.\n");
}
