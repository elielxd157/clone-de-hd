/* hdclone - preparar p/ outro hardware, verificar/restaurar */

/* ------------------------------------------------------------------ */
/* preparar o Windows para outro hardware (evitar tela azul 0x7B)      */
/* ------------------------------------------------------------------ */
/*
 * A tela azul INACCESSIBLE_BOOT_DEVICE (0x7B) acontece quando o driver da
 * controladora de disco do PC novo nao esta marcado para iniciar no boot.
 * Aqui os drivers de disco que ja vem no Windows passam a iniciar no boot
 * (Start = 0), no registro SYSTEM do disco restaurado/clonado:
 *   XP/2000/2003 : atapi, intelide, pciide           (ideia do "MergeIDE")
 *   Vista/7      : + msahci, iaStorV                 (KB922976 da Microsoft)
 *   8/8.1/10/11  : + storahci, stornvme e StartOverride zerado
 * So valores que ja existem sao alterados, no proprio lugar: nenhuma chave e
 * criada e a estrutura do registro nao muda.
 */

typedef struct {
    u8 *b;
    u64 len;
} Hive;

/* dados de uma celula (offset relativo ao inicio dos hbins, em 0x1000) */
static u8 *hv_at(Hive *h, u32 off, u32 need)
{
    u64 p = 0x1000ull + off;
    if (off == 0xFFFFFFFFu || p + 4 + need > h->len) return NULL;
    return h->b + p + 4;
}

static int hv_name_is(Hive *h, u32 nk_off, const char *name)
{
    u8 *nk = hv_at(h, nk_off, 0x4C), *nm;
    u32 nl, i, n = (u32)strlen(name);
    if (!nk || nk[0] != 'n' || nk[1] != 'k') return 0;
    nl = le16(nk + 0x48);
    if (!(nm = hv_at(h, nk_off, 0x4C + nl))) return 0;
    nm += 0x4C;
    if (le16(nk + 2) & 0x20) { /* nome em ASCII */
        if (nl != n) return 0;
        for (i = 0; i < n; i++)
            if (tolower(nm[i]) != tolower((u8)name[i])) return 0;
    } else { /* UTF-16 */
        if (nl != 2 * n) return 0;
        for (i = 0; i < n; i++)
            if (le16(nm + 2 * i) > 127 || tolower(le16(nm + 2 * i)) != tolower((u8)name[i])) return 0;
    }
    return 1;
}

static u32 hv_find_list(Hive *h, u32 list, const char *name, int depth)
{
    u8 *l = hv_at(h, list, 4);
    u32 cnt, i, step, r;
    if (!l || depth > 4) return 0;
    cnt = le16(l + 2);
    if (l[0] == 'r' && l[1] == 'i') {
        if (!hv_at(h, list, 4 + 4 * cnt)) return 0;
        for (i = 0; i < cnt; i++)
            if ((r = hv_find_list(h, le32(l + 4 + 4 * i), name, depth + 1))) return r;
        return 0;
    }
    if (l[0] == 'l' && l[1] == 'i') step = 4;
    else if (l[0] == 'l' && (l[1] == 'f' || l[1] == 'h')) step = 8;
    else return 0;
    if (!hv_at(h, list, 4 + step * cnt)) return 0;
    for (i = 0; i < cnt; i++) {
        u32 c = le32(l + 4 + step * i);
        if (hv_name_is(h, c, name)) return c;
    }
    return 0;
}

static u32 hv_sub(Hive *h, u32 nk, const char *name)
{
    u8 *n = hv_at(h, nk, 0x4C);
    if (!n || n[0] != 'n' || n[1] != 'k' || le32(n + 0x14) == 0) return 0;
    return hv_find_list(h, le32(n + 0x1C), name, 0);
}

static u32 hv_path(Hive *h, u32 nk, const char *path)
{
    char comp[128];
    while (*path && nk) {
        int n = 0;
        while (*path && *path != '\\' && n < 127) comp[n++] = *path++;
        comp[n] = 0;
        if (*path == '\\') path++;
        nk = hv_sub(h, nk, comp);
    }
    return nk;
}

/* percorre os valores REG_DWORD de uma chave; name = NULL para todos.
   Devolve quantos encontrou; offs/vals recebem a posicao no ARQUIVO e o valor. */
static int hv_dwords(Hive *h, u32 nk, const char *name, u64 *offs, u32 *vals, int max)
{
    u8 *n = hv_at(h, nk, 0x4C), *vl;
    u32 cnt, i;
    int found = 0;
    if (!n || n[0] != 'n' || n[1] != 'k') return 0;
    cnt = le32(n + 0x24);
    if (!cnt || cnt > 100000 || !(vl = hv_at(h, le32(n + 0x28), 4 * cnt))) return 0;
    for (i = 0; i < cnt && found < max; i++) {
        u32 vo = le32(vl + 4 * i), nl, sz, doff;
        u8 *vk = hv_at(h, vo, 0x14), *nm;
        if (!vk || vk[0] != 'v' || vk[1] != 'k') continue;
        nl = le16(vk + 2);
        sz = le32(vk + 4);
        if (le32(vk + 0x0C) != 4 || (sz & 0x7FFFFFFFu) != 4) continue; /* REG_DWORD de 4 bytes */
        if (!(nm = hv_at(h, vo, 0x14 + nl))) continue;
        if (name) {
            u32 k, L = (u32)strlen(name);
            if (!(le16(vk + 0x10) & 1) || nl != L) continue; /* nomes de valores procurados sao ASCII */
            for (k = 0; k < L && tolower(nm[0x14 + k]) == tolower((u8)name[k]); k++)
                ;
            if (k != L) continue;
        }
        if (sz & 0x80000000u) { /* dado guardado dentro do proprio vk */
            offs[found] = 0x1000ull + vo + 4 + 8;
        } else {
            doff = le32(vk + 8);
            if (!hv_at(h, doff, 4)) continue;
            offs[found] = 0x1000ull + doff + 4;
        }
        vals[found] = le32(h->b + offs[found]);
        found++;
    }
    return found;
}

typedef struct {
    u64 off;
    u32 val;
} Patch;

static const char *g_hw_services[] = {"atapi", "intelide", "pciide", "msahci", "iaStorV", "storahci", "stornvme", NULL};

/* calcula as alteracoes num registro SYSTEM carregado em memoria */
static int hwfix_hive(Hive *h, Patch *pt, int max, int *np, int *os)
{
    u32 root, cs, svc, k;
    int n, i, j, sets = 0;
    char nm[32];
    *np = 0;
    *os = 0;
    if (h->len < 0x2000 || memcmp(h->b, "regf", 4) != 0) return -1;
    root = le32(h->b + 0x24);
    for (n = 1; n <= 20; n++) {
        snprintf(nm, sizeof(nm), "ControlSet%03d", n);
        if (!(cs = hv_sub(h, root, nm)) || !(svc = hv_sub(h, cs, "Services"))) continue;
        sets++;
        if (!*os) *os = hv_sub(h, svc, "storahci") ? 3 : hv_sub(h, svc, "msahci") ? 2 : 1;
        for (i = 0; g_hw_services[i]; i++) {
            u64 offs[64];
            u32 vals[64], so;
            int c;
            if (!(k = hv_sub(h, svc, g_hw_services[i]))) continue;
            if (hv_dwords(h, k, "Start", offs, vals, 1) == 1 && vals[0] != 0 && *np < max) {
                printf("    %s\\Services\\%s: Start %u -> 0 (inicia no boot)\n", nm, g_hw_services[i], vals[0]);
                pt[*np].off = offs[0];
                pt[(*np)++].val = 0;
            }
            if ((so = hv_sub(h, k, "StartOverride")) && (c = hv_dwords(h, so, NULL, offs, vals, 64)) > 0)
                for (j = 0; j < c; j++)
                    if (vals[j] != 0 && *np < max) {
                        printf("    %s\\Services\\%s\\StartOverride: %u -> 0\n", nm, g_hw_services[i], vals[j]);
                        pt[*np].off = offs[j];
                        pt[(*np)++].val = 0;
                    }
        }
        if (*os == 1) {
            u32 cd = hv_path(h, cs, "Control\\CriticalDeviceDatabase");
            if (cd && !hv_sub(h, cd, "pci#cc_0101"))
                printf("    AVISO: %s sem a entrada generica de controladora IDE (pci#cc_0101) no\n"
                       "    CriticalDeviceDatabase; num PC muito diferente o XP ainda pode precisar do MergeIDE.\n", nm);
        }
    }
    return sets ? 0 : -1;
}

/* procura instalacoes do Windows no disco e aplica a correcao. Devolve quantas preparou. */
static int hwfix_disk(Dev *d)
{
    static const char *cands[] = {"Windows/System32/config/SYSTEM", "WINNT/system32/config/system", NULL};
    PTable *t = malloc(sizeof(PTable));
    int i, c, done = 0;
    if (!t) return 0;
    if (list_parts(d, t) != 0) {
        printf("  Tabela de particoes nao reconhecida.\n");
        free(t);
        return 0;
    }
    for (i = 0; i < t->n; i++) {
        Fs fs;
        if (fs_open(&fs, d, t->p[i].start, t->p[i].len) != 0) continue;
        for (c = 0; cands[c]; c++) {
            DirEnt e;
            FMap m;
            const char *why = NULL;
            Hive h;
            Patch pt[256];
            int np = 0, os = 0, k, ok = 1;
            if (fs_lookup(&fs, cands[c], &e) != 0 || e.is_dir) continue;
            printf("  Windows encontrado na particao em %llu MB (pasta \\%s)\n",
                   (unsigned long long)(t->p[i].start >> 20), c == 0 ? "Windows" : "WINNT");
            if (fs_fmap(&fs, &e, &m, &why) != 0 || m.resident || m.init_size < m.size || m.size > 512u * 1024 * 1024) {
                printf("    AVISO: o arquivo do registro esta num formato que nao pode ser alterado%s%s.\n",
                       why ? ": " : "", why ? why : "");
                fm_free(&m);
                continue;
            }
            h.len = m.size;
            h.b = malloc((size_t)h.len + 8);
            if (!h.b || fm_read(d, &m, 0, h.len, h.b) != 0) {
                printf("    AVISO: nao foi possivel ler o registro.\n");
                free(h.b);
                fm_free(&m);
                continue;
            }
            if (le32(h.b + 4) != le32(h.b + 8))
                printf("    Obs.: o registro tem alteracoes pendentes de gravacao (desligamento sem\n"
                       "    finalizar). A correcao foi aplicada, mas o Windows pode desfaze-la ao iniciar.\n");
            if (hwfix_hive(&h, pt, 256, &np, &os) != 0) {
                printf("    AVISO: registro SYSTEM nao reconhecido.\n");
                free(h.b);
                fm_free(&m);
                continue;
            }
            for (k = 0; k < np; k++) {
                u64 da, db;
                u8 v[4], back[4];
                put32(v, pt[k].val);
                if (fm_locate(&m, pt[k].off, &da) != 0 || fm_locate(&m, pt[k].off + 3, &db) != 0 || db != da + 3 ||
                    dev_write_any(d, da, 4, v) != 0 || dev_read_any(d, da, 4, back) != 0 || memcmp(back, v, 4) != 0) {
                    ok = 0;
                    break;
                }
            }
            printf("    %s: %d alteracao(oes) %s.\n",
                   os == 3 ? "Windows 8/10/11" : os == 2 ? "Windows Vista/7" : "Windows XP/2000/2003", np,
                   !ok ? "-- FALHA AO GRAVAR" : np ? "gravada(s)" : "(ja estava preparado)");
            if (os == 1)
                printf("    Dica: se o PC novo usa SATA em modo AHCI, mude no BIOS para IDE/Compativel\n"
                       "    (o XP nao tem driver AHCI proprio). Ex.: \"SATA Mode: IDE\".\n");
            else if (os == 2)
                printf("    Obs.: o Windows 7 nao tem driver NVMe proprio; para SSD NVMe instale a\n"
                       "    atualizacao KB2990941 antes do backup (ou use SATA).\n");
            if (ok) done++;
            free(h.b);
            fm_free(&m);
        }
        fs_close(&fs);
    }
    free(t);
    if (!done) printf("  Nenhuma instalacao do Windows foi preparada.\n");
    return done;
}

/* prepara um disco ja restaurado/clonado para outro hardware */
#ifdef HDCLONE_GUI
__attribute__((unused))
#endif
static void cmd_hwfix(const char *dst, Opts *o)
{
    Dev d;
    int n;
    if (dev_open(&d, dst, 1) != 0) die("nao foi possivel abrir o disco (execute como Administrador)");
    if (d.is_device && !d.size) die("nao foi possivel obter o tamanho do disco");
    if (!d.is_device) {
#ifndef _WIN32
        struct stat st;
        if (fstat(d.fd, &st) == 0) d.size = (u64)st.st_size;
#endif
    }
    printf("\nDisco: %s\n", d.name);
    if (d.disknum >= 0) {
        printf("Desmontando volumes...\n");
        if (lock_disk_volumes(d.disknum, 1) > 0 && !o->force)
            die("ha volumes em uso neste disco (o disco do Windows em uso nao pode ser alterado; use outro PC ou boot por pendrive)");
    }
    printf("\nPreparando o Windows para funcionar em outro hardware...\n");
    n = hwfix_disk(&d);
#ifdef _WIN32
    if (d.is_device) {
        DWORD br;
        FlushFileBuffers(d.h);
        DeviceIoControl(d.h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &br, NULL);
    }
#endif
    unlock_all();
    dev_close(&d);
    if (!n) die("nenhuma instalacao do Windows foi preparada");
    printf("\nPronto.\n");
}

/* ---- restauracao / verificacao em pipeline ---- */

typedef struct {
    Img *m;
    Dev *d;
    Opts *o;
    Hdr *h;
    u64 limit;
    u8 *zeros;
    ZSTD_DCtx *zd[8];
} RsCtx;

static int rs_produce(Pipe *p, Slot *sl)
{
    RsCtx *c = p->ctx;
    u8 ch[9];
    char msg[200];
    u64 off = sl->idx * CHUNK_SIZE;
    sl->len = (u32)((c->h->size - off) < CHUNK_SIZE ? (c->h->size - off) : CHUNK_SIZE);
    if (img_read(c->m, ch, 9) != 0) {
        snprintf(msg, sizeof(msg), "imagem incompleta: termina no bloco %llu de %llu (falta alguma parte?)",
                 (unsigned long long)sl->idx, (unsigned long long)c->h->nchunks);
        pipe_fail(p, msg);
        return -1;
    }
    sl->type = ch[0];
    sl->stored = le32(ch + 1);
    sl->crc = le32(ch + 5);
    if (sl->type > CH_ZSTD || (sl->type == CH_RAW && sl->stored != sl->len) || sl->stored > CBUF_SIZE) {
        snprintf(msg, sizeof(msg), "bloco %llu invalido (imagem corrompida)", (unsigned long long)sl->idx);
        pipe_fail(p, msg);
        return -1;
    }
    if ((sl->type == CH_DEFLATE || sl->type == CH_RAW || sl->type == CH_ZSTD) &&
        img_read(c->m, sl->cbuf, sl->stored) != 0) {
        pipe_fail(p, "imagem corrompida/incompleta (falta alguma parte?)");
        return -1;
    }
    return 0;
}

static int rs_work(Pipe *p, Slot *sl, int w)
{
    RsCtx *c = p->ctx;
    char msg[200];
    switch (sl->type) {
    case CH_ZERO: sl->out = c->zeros; return 0;
    case CH_FREE: sl->out = c->o->zero_free ? c->zeros : NULL; return 0;
    case CH_RAW: sl->out = sl->cbuf; break;
    case CH_ZSTD:
        if (ZSTD_decompressDCtx(c->zd[w], sl->buf, CHUNK_SIZE, sl->cbuf, sl->stored) != sl->len) goto bad;
        sl->out = sl->buf;
        break;
    default: {
        uLongf ul = CHUNK_SIZE;
        if (uncompress(sl->buf, &ul, sl->cbuf, sl->stored) != Z_OK || ul != sl->len) goto bad;
        sl->out = sl->buf;
    }
    }
    if ((u32)crc32(0, sl->out, sl->len) == sl->crc) return 0;
    snprintf(msg, sizeof(msg), "CRC invalido no bloco %llu (imagem corrompida)", (unsigned long long)sl->idx);
    pipe_fail(p, msg);
    return -1;
bad:
    snprintf(msg, sizeof(msg), "dados corrompidos no bloco %llu", (unsigned long long)sl->idx);
    pipe_fail(p, msg);
    return -1;
}

static int rs_consume(Pipe *p, Slot *sl)
{
    RsCtx *c = p->ctx;
    u64 off = sl->idx * CHUNK_SIZE;
    if (c->d && sl->out && off < c->limit &&
        dev_pwrite(c->d, sl->out, off + sl->len > c->limit ? (u32)(c->limit - off) : sl->len, off) != 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "falha de escrita no disco de destino na posicao %llu MB",
                 (unsigned long long)(off >> 20));
        pipe_fail(p, msg);
        return -1;
    }
    p->done_bytes += sl->len;
    return 0;
}

/* processa a imagem; se d != NULL grava no disco (ate "limit" bytes) */
static void process_image(const char *path, Dev *d, Opts *o, Hdr *h, Img *m, u64 limit)
{
    u8 ftr[FTR_SIZE];
    Pipe pp;
    RsCtx rc;
    int k, r;
    (void)path;
    memset(&rc, 0, sizeof(rc));
    rc.m = m;
    rc.d = d;
    rc.o = o;
    rc.h = h;
    rc.limit = limit;
    rc.zeros = xalloc(CHUNK_SIZE);
    pipe_init(&pp, h->nchunks, h->size);
    pp.produce = rs_produce;
    pp.work = rs_work;
    pp.consume = rs_consume;
    pp.ctx = &rc;
    for (k = 0; k < pp.N; k++) rc.zd[k] = ZSTD_createDCtx();
    r = pipe_run(&pp);
    for (k = 0; k < pp.N; k++) ZSTD_freeDCtx(rc.zd[k]);
    pipe_free(&pp);
    xfree(rc.zeros);
    if (r != 0) die(pp.err);
    if (img_read(m, ftr, FTR_SIZE) != 0 || memcmp(ftr, "HDCEND", 6) != 0 ||
        le32(ftr + 44) != (u32)crc32(0, ftr, 44))
        die("rodape da imagem ausente ou corrompido (backup incompleto?)");
    if (le64(ftr + 32))
        printf("\n  Obs.: o disco original tinha %llu setores ilegiveis (gravados como zero).",
               (unsigned long long)le64(ftr + 32));
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

    if (o->verify) { /* confere a imagem inteira ANTES de apagar qualquer coisa no destino */
        printf("\nVerificando a imagem antes de gravar (o destino ainda nao foi alterado)...\n");
        cmd_verify(path);
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
    if (o->hwfix) {
        printf("\nPreparando o Windows para funcionar em outro hardware...\n");
        hwfix_disk(&d);
    }
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
