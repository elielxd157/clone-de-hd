/*
 * hdclone-gui - interface grafica do hdclone (Windows XP ate Windows 11).
 *
 * Reaproveita todo o nucleo de hdclone.c. A operacao roda numa thread
 * separada; mensagens, progresso e erros chegam a janela via PostMessage.
 */

#define UNICODE
#define _UNICODE
#define HDCLONE_GUI
#include "hdclone.c"
#include <setjmp.h>
#include <stdarg.h>
#include <wchar.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#define APP_TITLE L"hdclone - Clone de HD"

enum {
    ID_MODE_BACKUP = 100, ID_MODE_RESTORE, ID_MODE_VERIFY, ID_MODE_EXPLORE, ID_KEEPSYS,
    ID_LBL_DISK, ID_REFRESH, ID_LIST,
    ID_LBL_IMG, ID_PATH, ID_BROWSE, ID_IMGINFO,
    ID_GRP_OPT, ID_LBL_COMP, ID_COMP, ID_SPLIT, ID_RAW, ID_ZEROFREE, ID_VSS,
    ID_START, ID_CANCEL, ID_PROGRESS, ID_STATUS, ID_LOG, ID_GRP_MODE
};

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_PROG (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

#define MODE_BACKUP 0
#define MODE_RESTORE 1
#define MODE_VERIFY 2
#define MODE_EXPLORE 3

typedef struct {
    int mode;
    char disk[16];
    char path[MAX_PATH * 2];
    Opts o;
} Job;

typedef struct {
    u64 done, total;
    double speed;
    unsigned long eta;
} Prog;

static HWND g_hwnd, g_list, g_log, g_prog, g_status, g_path, g_imginfo, g_comp;
static HFONT g_font, g_bold;
static int g_mode = MODE_BACKUP;
static int g_running, g_close_after, g_job_sysdisk;
static DiskInfo g_disks[32];
static int g_ndisks;
static Job g_job;
static jmp_buf g_jmp;
static char g_errmsg[600];
static double g_scale = 1.0;
static HWND g_notify;            /* janela que recebe o progresso */
static HWND g_xw;                /* janela "Recuperar arquivos" */
static VolInfo g_vols[32];       /* particoes NTFS da imagem aberta para explorar */
static int g_nvols;
static void open_explorer(void);

/* ------------------------------------------------------------------ */
/* ganchos chamados pelo nucleo (na thread de trabalho)                */
/* ------------------------------------------------------------------ */

static void ui_log(const char *fmt, ...)
{
    char a[4096], b[8192];
    int i, j = 0, n;
    wchar_t *w;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a, sizeof(a), fmt, ap);
    va_end(ap);
    for (i = 0; a[i] && j < (int)sizeof(b) - 2; i++) { /* \n -> \r\n para o controle EDIT */
        if (a[i] == '\r') continue;
        if (a[i] == '\n') b[j++] = '\r';
        b[j++] = a[i];
    }
    b[j] = 0;
    n = MultiByteToWideChar(CP_ACP, 0, b, -1, NULL, 0);
    w = malloc(n * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_ACP, 0, b, -1, w, n);
    PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)w);
}

static void ui_progress(u64 done, u64 total, double speed, unsigned long eta)
{
    Prog *p = malloc(sizeof(Prog));
    if (!p) return;
    p->done = done;
    p->total = total;
    p->speed = speed;
    p->eta = eta;
    PostMessageW(g_notify ? g_notify : g_hwnd, WM_APP_PROG, 0, (LPARAM)p);
}

static void ui_fatal(const char *msg)
{
    strncpy(g_errmsg, msg, sizeof(g_errmsg) - 1);
    ui_log("\nERRO: %s\n", msg);
    cleanup_all();
    longjmp(g_jmp, 1);
}

static DWORD WINAPI worker(LPVOID arg)
{
    Job *j = arg;
    int ok = 0;
    g_errmsg[0] = 0;
    if (setjmp(g_jmp) == 0) {
        if (j->mode == MODE_BACKUP) cmd_backup(j->disk, j->path, &j->o);
        else if (j->mode == MODE_RESTORE) cmd_restore(j->path, j->disk, &j->o);
        else if (j->mode == MODE_EXPLORE) {
            printf("Abrindo a imagem para recuperar arquivos...\n");
            open_virtual(j->path);
            g_nvols = image_volumes(&g_vdev, g_vols, 32);
            if (!g_nvols) die("a imagem nao tem particoes NTFS (a recuperacao de arquivos funciona com NTFS)");
            printf("\n%d particao(oes) NTFS encontrada(s).\n", g_nvols);
        } else cmd_verify(j->path);
        ok = 1;
    }
    PostMessageW(g_hwnd, WM_APP_DONE, ok, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* utilitarios da interface                                            */
/* ------------------------------------------------------------------ */

static int S(int v) { return (int)(v * g_scale + 0.5); }

static HWND ctl(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id)
{
    return CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, WC_LISTVIEWW) ? WS_EX_CLIENTEDGE : 0,
                           cls, txt, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, S(x), S(y), S(w), S(h),
                           g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void fmt_sizew(u64 b, wchar_t *out)
{
    char t[32];
    fmt_size(b, t);
    MultiByteToWideChar(CP_ACP, 0, t, -1, out, 32);
}

static void msgbox(const wchar_t *txt, UINT icon)
{
    MessageBoxW(g_hwnd, txt, APP_TITLE, MB_OK | icon);
}

/* caminho da caixa de texto convertido para ANSI (o nucleo usa APIs "A") */
static int get_path(char *out, int n)
{
    wchar_t w[MAX_PATH * 2];
    BOOL bad = FALSE;
    GetWindowTextW(g_path, w, MAX_PATH * 2);
    /* tira espacos e aspas das pontas */
    {
        wchar_t *p = w, *e;
        while (*p == L' ' || *p == L'"') p++;
        e = p + wcslen(p);
        while (e > p && (e[-1] == L' ' || e[-1] == L'"')) *--e = 0;
        memmove(w, p, (wcslen(p) + 1) * sizeof(wchar_t));
    }
    if (!w[0]) return 0;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, n, NULL, &bad);
    if (bad) {
        msgbox(L"O caminho do arquivo contém caracteres não suportados.\n"
               L"Use uma pasta/nome de arquivo sem caracteres especiais.", MB_ICONERROR);
        return 0;
    }
    return 1;
}

/* le o cabecalho da imagem sem abortar (para mostrar informacoes) */
static int peek_header(const char *path, Hdr *h)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    u8 b[HDR_SIZE];
    DWORD r = 0;
    if (f == INVALID_HANDLE_VALUE) return -1;
    ReadFile(f, b, HDR_SIZE, &r, NULL);
    CloseHandle(f);
    if (r != HDR_SIZE || memcmp(b, "HDCLONE", 8) != 0 || le32(b + 508) != (u32)crc32(0, b, 508)) return -2;
    h->version = le32(b + 8);
    if (h->version < 1 || h->version > 2) return -3;
    h->sector = le32(b + 12);
    h->flags = le32(b + 20);
    h->size = le64(b + 24);
    h->created = le64(b + 40);
    memcpy(h->src, b + 48, 128);
    h->src[128] = 0;
    return 0;
}

static void update_imginfo(void)
{
    char path[MAX_PATH * 2];
    wchar_t t[400], sz[32], dt[64] = L"?";
    Hdr h;
    int r;
    if (g_mode == MODE_BACKUP) { SetWindowTextW(g_imginfo, L""); return; }
    {   /* sem caixas de erro aqui: le o texto direto */
        wchar_t w[MAX_PATH * 2];
        GetWindowTextW(g_path, w, MAX_PATH * 2);
        if (!w[0]) { SetWindowTextW(g_imginfo, L""); return; }
        WideCharToMultiByte(CP_ACP, 0, w, -1, path, sizeof(path), NULL, NULL);
    }
    r = peek_header(path, &h);
    if (r == -1) { SetWindowTextW(g_imginfo, L"Arquivo não encontrado."); return; }
    if (r == -2) { SetWindowTextW(g_imginfo, L"Este arquivo não é uma imagem do hdclone válida."); return; }
    if (r == -3) { SetWindowTextW(g_imginfo, L"Imagem criada por uma versão mais nova do hdclone: atualize o programa."); return; }
    fmt_sizew(h.size, sz);
    {
        time_t tt = (time_t)h.created;
        struct tm *tm = localtime(&tt);
        if (tm) wcsftime(dt, 64, L"%d/%m/%Y %H:%M", tm);
    }
    swprintf(t, 400, L"Imagem de um disco de %ls, criada em %ls (%ls, %ls).", sz, dt,
             (h.flags & 1) ? L"somente dados usados" : L"setor a setor",
             h.version >= 2 ? L"zstd" : L"zlib");
    SetWindowTextW(g_imginfo, t);
}

static void refresh_disks(void)
{
    int n, sysd = system_disk(), sel = -1, i;
    LVITEMW it;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    ListView_DeleteAllItems(g_list);
    g_ndisks = 0;
    for (n = 0; n < 32; n++) {
        DiskInfo *d = &g_disks[g_ndisks];
        wchar_t t[160];
        if (disk_info(n, sysd, d) != 0) continue;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = g_ndisks;
        it.lParam = g_ndisks;
        swprintf(t, 160, L"Disco %d", n);
        it.pszText = t;
        i = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        fmt_sizew(d->size, t);
        ListView_SetItemText(g_list, i, 1, t);
        MultiByteToWideChar(CP_ACP, 0, d->model[0] ? d->model : "(desconhecido)", -1, t, 160);
        ListView_SetItemText(g_list, i, 2, t);
        MultiByteToWideChar(CP_ACP, 0, d->letters[0] ? d->letters : "-", -1, t, 160);
        ListView_SetItemText(g_list, i, 3, t);
        ListView_SetItemText(g_list, i, 4, d->is_system ? L"Windows em uso" : L"");
        g_ndisks++;
    }
    SetCursor(old);
    (void)sel;
    if (!g_ndisks)
        ui_log("Nenhum disco encontrado. Execute o programa como Administrador.\n");
}

static DiskInfo *selected_disk(void)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    LVITEMW it;
    if (i < 0) return NULL;
    memset(&it, 0, sizeof(it));
    it.mask = LVIF_PARAM;
    it.iItem = i;
    SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&it);
    return &g_disks[it.lParam];
}

static void set_mode(int m)
{
    static const wchar_t *lbl_disk[] = {
        L"1.  Escolha o disco de ORIGEM (o que será copiado):",
        L"2.  Escolha o disco de DESTINO (será totalmente apagado!):",
        L"(não é necessário escolher disco para verificar)",
        L"(não é necessário escolher disco para recuperar arquivos)"};
    static const wchar_t *lbl_img[] = {
        L"2.  Onde salvar a imagem:",
        L"1.  Arquivo de imagem a restaurar (.hdc):",
        L"Arquivo de imagem a verificar (.hdc):",
        L"Imagem (.hdc) de onde recuperar arquivos:"};
    g_mode = m;
    SetDlgItemTextW(g_hwnd, ID_LBL_DISK, lbl_disk[m]);
    SetDlgItemTextW(g_hwnd, ID_LBL_IMG, lbl_img[m]);
    EnableWindow(g_list, m == MODE_BACKUP || m == MODE_RESTORE);
    EnableWindow(GetDlgItem(g_hwnd, ID_REFRESH), m == MODE_BACKUP || m == MODE_RESTORE);
    EnableWindow(GetDlgItem(g_hwnd, ID_KEEPSYS), m == MODE_BACKUP);
    EnableWindow(g_comp, m == MODE_BACKUP);
    EnableWindow(GetDlgItem(g_hwnd, ID_LBL_COMP), m == MODE_BACKUP);
    EnableWindow(GetDlgItem(g_hwnd, ID_SPLIT), m == MODE_BACKUP);
    EnableWindow(GetDlgItem(g_hwnd, ID_RAW), m == MODE_BACKUP);
    EnableWindow(GetDlgItem(g_hwnd, ID_ZEROFREE), m == MODE_RESTORE);
    EnableWindow(GetDlgItem(g_hwnd, ID_VSS), m == MODE_BACKUP);
    SetDlgItemTextW(g_hwnd, ID_START, m == MODE_BACKUP    ? L"Iniciar backup"
                                      : m == MODE_RESTORE ? L"Iniciar restauração"
                                      : m == MODE_VERIFY  ? L"Verificar"
                                                          : L"Abrir imagem");
    update_imginfo();
}

static void set_running(int r)
{
    int ids[] = {ID_MODE_BACKUP, ID_MODE_RESTORE, ID_MODE_VERIFY, ID_MODE_EXPLORE, ID_PATH, ID_BROWSE, ID_START};
    int i;
    g_running = r;
    for (i = 0; i < (int)(sizeof(ids) / sizeof(ids[0])); i++) EnableWindow(GetDlgItem(g_hwnd, ids[i]), !r);
    EnableWindow(GetDlgItem(g_hwnd, ID_CANCEL), r);
    if (r) {
        EnableWindow(g_list, FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_REFRESH), FALSE);
        EnableWindow(g_comp, FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_SPLIT), FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_RAW), FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_ZEROFREE), FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_VSS), FALSE);
        EnableWindow(GetDlgItem(g_hwnd, ID_KEEPSYS), FALSE);
    } else {
        set_mode(g_mode);
        SetWindowTextW(g_hwnd, APP_TITLE);
    }
}

static void browse(void)
{
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW of;
    BOOL ok;
    GetWindowTextW(g_path, file, MAX_PATH * 2);
    memset(&of, 0, sizeof(of));
    of.lStructSize = OPENFILENAME_SIZE_VERSION_400W; /* tamanho compativel com XP */
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"Imagens do hdclone (*.hdc)\0*.hdc\0Todos os arquivos (*.*)\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 2;
    of.lpstrDefExt = L"hdc";
    if (g_mode == MODE_BACKUP) {
        of.lpstrTitle = L"Salvar imagem do disco como";
        of.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        ok = GetSaveFileNameW(&of);
    } else {
        of.lpstrTitle = L"Abrir imagem do disco";
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        ok = GetOpenFileNameW(&of);
    }
    if (ok) SetWindowTextW(g_path, file);
}

static void start(void)
{
    DiskInfo *d = NULL;
    wchar_t t[1200], sz[32], model[160], letters[160];
    int r;

    memset(&g_job, 0, sizeof(g_job));
    g_job.mode = g_mode;
    g_job.o.yes = 1;
    g_job.o.level = 6;
    g_job.o.split = 0;

    if (g_mode == MODE_BACKUP || g_mode == MODE_RESTORE) {
        d = selected_disk();
        if (!d) {
            msgbox(g_mode == MODE_BACKUP ? L"Selecione na lista o disco que será copiado."
                                         : L"Selecione na lista o disco de destino.", MB_ICONINFORMATION);
            return;
        }
        sprintf(g_job.disk, "%d", d->num);
        fmt_sizew(d->size, sz);
        MultiByteToWideChar(CP_ACP, 0, d->model[0] ? d->model : "(desconhecido)", -1, model, 160);
        MultiByteToWideChar(CP_ACP, 0, d->letters[0] ? d->letters : "nenhuma", -1, letters, 160);
    }
    if (!get_path(g_job.path, sizeof(g_job.path))) {
        if (GetWindowTextLengthW(g_path) == 0)
            msgbox(g_mode == MODE_BACKUP ? L"Informe onde salvar a imagem (botão Procurar...)."
                                         : L"Escolha o arquivo de imagem (botão Procurar...).", MB_ICONINFORMATION);
        return;
    }

    if (g_mode == MODE_BACKUP) {
        /* Rapida/Normal/Maxima = zstd 1/3/9; Compativel = zlib 6 (lido por versoes antigas) */
        static const int lv[] = {1, 3, 9, 6};
        int ci = (int)SendMessageW(g_comp, CB_GETCURSEL, 0, 0);
        if (ci < 0 || ci > 3) ci = 1;
        g_job.o.level = lv[ci];
        g_job.o.compat = ci == 3;
        g_job.o.split = IsDlgButtonChecked(g_hwnd, ID_SPLIT) ? 4000ull * 1024 * 1024 : 0;
        g_job.o.raw = IsDlgButtonChecked(g_hwnd, ID_RAW) == BST_CHECKED;
        g_job.o.no_vss = IsDlgButtonChecked(g_hwnd, ID_VSS) != BST_CHECKED;
        g_job.o.keep_sys = IsDlgButtonChecked(g_hwnd, ID_KEEPSYS) != BST_CHECKED;
        if (!strchr(g_job.path, '.') || strrchr(g_job.path, '.') < strrchr(g_job.path, '\\')) {
            strcat(g_job.path, ".hdc");
        }
        if (GetFileAttributesA(g_job.path) != INVALID_FILE_ATTRIBUTES) {
            if (MessageBoxW(g_hwnd, L"O arquivo de imagem já existe. Deseja substituí-lo?", APP_TITLE,
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return;
        }
        g_job_sysdisk = d->is_system;
        if (d->is_system && g_job.o.no_vss) {
            if (MessageBoxW(g_hwnd,
                            L"Este é o disco do Windows que está em uso e o VSS está desligado.\n\n"
                            L"A cópia vai funcionar, mas arquivos abertos durante o backup podem "
                            L"ficar inconsistentes na imagem. Recomendado: marque a opção "
                            L"\"Usar VSS\".\n\nContinuar mesmo assim?",
                            APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                return;
        }
    } else if (g_mode == MODE_RESTORE) {
        Hdr h;
        wchar_t isz[32];
        r = peek_header(g_job.path, &h);
        if (r != 0) {
            msgbox(r == -1 ? L"Arquivo de imagem não encontrado."
                   : r == -3 ? L"Esta imagem foi criada por uma versão mais nova do hdclone. Atualize o programa."
                             : L"Este arquivo não é uma imagem do hdclone válida.",
                   MB_ICONERROR);
            return;
        }
        fmt_sizew(h.size, isz);
        if (d->is_system) {
            msgbox(L"Não é possível restaurar sobre o disco do Windows que está em uso.\n\n"
                   L"Para restaurar o disco do sistema, inicie o computador por um pendrive "
                   L"WinPE / Hiren's BootCD PE e execute o hdclone a partir dele.", MB_ICONERROR);
            return;
        }
        g_job.o.zero_free = IsDlgButtonChecked(g_hwnd, ID_ZEROFREE) == BST_CHECKED;
        swprintf(t, 1200,
                 L"ATENÇÃO: TODOS OS DADOS DO DISCO ABAIXO SERÃO APAGADOS!\n\n"
                 L"    Disco %d  -  %ls\n    %ls\n    Unidades: %ls\n\n"
                 L"Ele será substituído pela imagem (disco original de %ls).\n%ls"
                 L"Esta operação não pode ser desfeita.\n\nTem certeza que deseja continuar?",
                 d->num, sz, model, letters, isz,
                 d->size < h.size
                     ? L"\nO disco de destino é MENOR que o original: a última partição será reduzida, "
                       L"se os dados couberem. O hdclone confere isso antes de gravar; se não couber, "
                       L"ele avisa o tamanho mínimo e o disco não é alterado.\n\n"
                     : L"");
        if (MessageBoxW(g_hwnd, t, APP_TITLE, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    } else if (g_mode == MODE_EXPLORE) {
        Hdr h;
        r = peek_header(g_job.path, &h);
        if (r != 0) {
            msgbox(r == -1 ? L"Arquivo de imagem não encontrado."
                   : r == -3 ? L"Esta imagem foi criada por uma versão mais nova do hdclone. Atualize o programa."
                             : L"Este arquivo não é uma imagem do hdclone válida.",
                   MB_ICONERROR);
            return;
        }
    }

    SetWindowTextW(g_log, L"");
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_status, L"Iniciando...");
    g_cancel = 0;
    set_running(1);
    {
        DWORD tid;
        HANDLE th = CreateThread(NULL, 0, worker, &g_job, 0, &tid);
        if (!th) { set_running(0); msgbox(L"Não foi possível iniciar a operação.", MB_ICONERROR); return; }
        CloseHandle(th);
    }
}

static void cancel(int ask)
{
    if (!g_running) return;
    if (ask) {
        const wchar_t *m = g_job.mode == MODE_RESTORE
            ? L"Cancelar a restauração?\n\nO disco de destino ficará incompleto e não vai funcionar "
              L"até ser restaurado novamente."
            : L"Cancelar a operação?";
        if (MessageBoxW(g_hwnd, m, APP_TITLE, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }
    g_cancel = 1;
    SetWindowTextW(g_status, L"Cancelando...");
}

/* ------------------------------------------------------------------ */
/* janela                                                              */
/* ------------------------------------------------------------------ */

static BOOL CALLBACK set_font_cb(HWND h, LPARAM f)
{
    SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static void create_controls(void)
{
    LVCOLUMNW c;
    int i;
    static const wchar_t *cols[] = {L"Disco", L"Tamanho", L"Modelo", L"Unidades", L"Observação"};
    static const int widths[] = {70, 80, 230, 90, 110};

    ctl(L"BUTTON", L"O que deseja fazer?", BS_GROUPBOX, 10, 6, 620, 48, ID_GRP_MODE);
    ctl(L"BUTTON", L"Backup (disco → imagem)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 20, 26, 160, 20, ID_MODE_BACKUP);
    ctl(L"BUTTON", L"Restaurar (imagem → disco)", BS_AUTORADIOBUTTON, 182, 26, 170, 20, ID_MODE_RESTORE);
    ctl(L"BUTTON", L"Verificar imagem", BS_AUTORADIOBUTTON, 356, 26, 118, 20, ID_MODE_VERIFY);
    ctl(L"BUTTON", L"Recuperar arquivos", BS_AUTORADIOBUTTON, 478, 26, 148, 20, ID_MODE_EXPLORE);
    CheckDlgButton(g_hwnd, ID_MODE_BACKUP, BST_CHECKED);

    ctl(L"STATIC", L"", SS_LEFT | WS_GROUP, 12, 66, 500, 18, ID_LBL_DISK);
    ctl(L"BUTTON", L"Atualizar lista", BS_PUSHBUTTON | WS_TABSTOP, 520, 61, 110, 24, ID_REFRESH);
    g_list = ctl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 10, 88, 620, 120, ID_LIST);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    for (i = 0; i < 5; i++) {
        memset(&c, 0, sizeof(c));
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (wchar_t *)cols[i];
        c.cx = S(widths[i]);
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }

    ctl(L"STATIC", L"", SS_LEFT, 12, 220, 500, 18, ID_LBL_IMG);
    g_path = ctl(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 10, 240, 510, 24, ID_PATH);
    ctl(L"BUTTON", L"Procurar...", BS_PUSHBUTTON | WS_TABSTOP, 530, 240, 100, 24, ID_BROWSE);
    g_imginfo = ctl(L"STATIC", L"", SS_LEFT, 12, 268, 618, 18, ID_IMGINFO);

    ctl(L"BUTTON", L"Opções", BS_GROUPBOX, 10, 290, 620, 124, ID_GRP_OPT);
    ctl(L"STATIC", L"Compressão:", SS_LEFT, 24, 312, 80, 18, ID_LBL_COMP);
    g_comp = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 104, 308, 180, 140, ID_COMP);
    SendMessageW(g_comp, CB_ADDSTRING, 0, (LPARAM)L"Rápida (arquivo maior)");
    SendMessageW(g_comp, CB_ADDSTRING, 0, (LPARAM)L"Normal (recomendado)");
    SendMessageW(g_comp, CB_ADDSTRING, 0, (LPARAM)L"Máxima (mais lento)");
    SendMessageW(g_comp, CB_ADDSTRING, 0, (LPARAM)L"Compatível com versões antigas (zlib, mais lento)");
    SendMessageW(g_comp, CB_SETDROPPEDWIDTH, S(300), 0);
    SendMessageW(g_comp, CB_SETCURSEL, 1, 0);
    ctl(L"BUTTON", L"Dividir em partes de 4 GB (para pendrive FAT32)", BS_AUTOCHECKBOX | WS_TABSTOP, 290, 310, 330, 20, ID_SPLIT);
    CheckDlgButton(g_hwnd, ID_SPLIT, BST_CHECKED);
    ctl(L"BUTTON", L"Copiar setor a setor (inclui espaço livre, imagem maior)", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 336, 330, 20, ID_RAW);
    ctl(L"BUTTON", L"Zerar espaço livre no destino", BS_AUTOCHECKBOX | WS_TABSTOP, 400, 336, 220, 20, ID_ZEROFREE);
    ctl(L"BUTTON", L"Usar VSS: copiar com o Windows ligado de forma consistente (recomendado)",
        BS_AUTOCHECKBOX | WS_TABSTOP, 24, 362, 590, 20, ID_VSS);
    ctl(L"BUTTON", L"Deixar de fora pagefile.sys e hiberfil.sys (o Windows recria; imagem bem menor)",
        BS_AUTOCHECKBOX | WS_TABSTOP, 24, 388, 590, 20, ID_KEEPSYS);
    CheckDlgButton(g_hwnd, ID_KEEPSYS, BST_CHECKED);
    CheckDlgButton(g_hwnd, ID_VSS, BST_CHECKED);

    ctl(L"BUTTON", L"Iniciar backup", BS_DEFPUSHBUTTON | WS_TABSTOP, 10, 424, 160, 30, ID_START);
    ctl(L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, 180, 424, 100, 30, ID_CANCEL);
    g_status = ctl(L"STATIC", L"Pronto.", SS_LEFT | SS_ENDELLIPSIS, 292, 431, 338, 20, ID_STATUS);
    g_prog = ctl(PROGRESS_CLASSW, L"", PBS_SMOOTH, 10, 462, 620, 20, ID_PROGRESS);
    SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
    g_log = ctl(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 10, 490, 620, 150, ID_LOG);
    SendMessageW(g_log, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);

    EnumChildWindows(g_hwnd, set_font_cb, (LPARAM)g_font);
    SendMessageW(GetDlgItem(g_hwnd, ID_START), WM_SETFONT, (WPARAM)g_bold, TRUE);
    set_mode(MODE_BACKUP);
    refresh_disks();
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = h;
        create_controls();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MODE_BACKUP: set_mode(MODE_BACKUP); break;
        case ID_MODE_RESTORE: set_mode(MODE_RESTORE); break;
        case ID_MODE_VERIFY: set_mode(MODE_VERIFY); break;
        case ID_MODE_EXPLORE: set_mode(MODE_EXPLORE); break;
        case ID_REFRESH: refresh_disks(); break;
        case ID_BROWSE: browse(); break;
        case ID_START: start(); break;
        case ID_CANCEL: cancel(1); break;
        case ID_PATH:
            if (HIWORD(wp) == EN_CHANGE) update_imginfo();
            break;
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_imginfo) {
            SetTextColor((HDC)wp, RGB(0, 70, 150));
            SetBkMode((HDC)wp, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_APP_LOG: {
        wchar_t *w = (wchar_t *)lp;
        int n = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, n, n);
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)w);
        free(w);
        return 0;
    }
    case WM_APP_PROG: {
        Prog *p = (Prog *)lp;
        wchar_t t[200];
        int pm = p->total ? (int)((double)p->done * 1000.0 / (double)p->total) : 0;
        SendMessageW(g_prog, PBM_SETPOS, pm, 0);
        swprintf(t, 200, L"%.1f%%   %llu de %llu MB   %.1f MB/s   restante %02lu:%02lu:%02lu",
                 pm / 10.0, (unsigned long long)(p->done >> 20), (unsigned long long)(p->total >> 20),
                 p->speed, p->eta / 3600, (p->eta / 60) % 60, p->eta % 60);
        SetWindowTextW(g_status, t);
        swprintf(t, 200, L"%d%% - hdclone", pm / 10);
        SetWindowTextW(g_hwnd, t);
        free(p);
        return 0;
    }
    case WM_APP_DONE: {
        int ok = (int)wp;
        int mode = g_job.mode;
        set_running(0);
        if (ok && mode == MODE_EXPLORE) {
            SendMessageW(g_prog, PBM_SETPOS, 1000, 0);
            SetWindowTextW(g_status, L"Imagem aberta.");
            if (!g_close_after) open_explorer();
            if (g_close_after) DestroyWindow(h);
            return 0;
        }
        if (ok) {
            SendMessageW(g_prog, PBM_SETPOS, 1000, 0);
            SetWindowTextW(g_status, L"Concluído com sucesso.");
            if (!g_close_after && mode == MODE_BACKUP && !g_vss_used && g_job_sysdisk) {
                wchar_t t[900], note[300];
                MultiByteToWideChar(CP_ACP, 0, g_vss_note[0] ? g_vss_note : "VSS desligado", -1, note, 300);
                swprintf(t, 900, L"Backup concluído, mas SEM VSS:\n%ls\n\n"
                                 L"Como este é o disco do Windows em uso, arquivos abertos durante a "
                                 L"cópia podem estar inconsistentes na imagem.", note);
                msgbox(t, MB_ICONWARNING);
            } else if (!g_close_after)
                msgbox(mode == MODE_BACKUP
                           ? (g_vss_used ? L"Backup concluído com sucesso!\n\nA cópia foi feita com VSS: a imagem "
                                           L"é consistente mesmo com o Windows ligado.\n\nDica: use \"Verificar "
                                           L"imagem\" para conferir o arquivo antes de precisar dele."
                                         : L"Backup concluído com sucesso!\n\nDica: use \"Verificar imagem\" "
                                           L"para conferir o arquivo antes de precisar dele.")
                       : mode == MODE_RESTORE
                           ? (g_shrunk ? L"Restauração concluída com sucesso!\n\nA última partição foi reduzida "
                                         L"para caber no disco. No primeiro boot o Windows pode fazer uma "
                                         L"verificação de disco (chkdsk): deixe terminar, é normal."
                                       : L"Restauração concluída com sucesso!")
                                              : L"Imagem verificada: está íntegra.",
                       MB_ICONINFORMATION);
        } else {
            wchar_t t[800], e[600];
            MultiByteToWideChar(CP_ACP, 0, g_errmsg, -1, e, 600);
            SetWindowTextW(g_status, g_cancel ? L"Cancelado." : L"Falhou.");
            if (!g_close_after) {
                swprintf(t, 800, L"A operação não foi concluída:\n\n%ls", e);
                msgbox(t, g_cancel ? MB_ICONINFORMATION : MB_ICONERROR);
            }
        }
        if (mode == MODE_RESTORE) refresh_disks();
        if (g_close_after) DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE:
        if (g_running) {
            if (MessageBoxW(h, L"Uma operação está em andamento. Deseja cancelá-la e sair?", APP_TITLE,
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                g_close_after = 1;
                cancel(0);
            }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* janela "Recuperar arquivos"                                         */
/* ------------------------------------------------------------------ */

enum { IDX_LBLVOL = 300, IDX_VOL, IDX_UP, IDX_WHERE, IDX_LIST, IDX_EXTRACT, IDX_XCANCEL, IDX_CLOSE, IDX_XPROG, IDX_XSTATUS };
#define WM_APP_XDONE (WM_APP + 4)
#define X_TITLE L"hdclone - Recuperar arquivos"

static HWND g_xlist, g_xvol, g_xwhere, g_xprog, g_xstatus;
static Ntfs g_nt;
static int g_nt_ok, g_xrunning, g_xclose_after;
static DirList g_cur;
static u64 g_xrec[256];      /* pilha de pastas abertas */
static int g_xlen[256];      /* tamanho do texto do caminho em cada nivel */
static int g_xdepth;
static wchar_t g_xpath[4096];
static DirEnt *g_xsel;
static int g_nxsel;
static wchar_t g_xdest[MAX_PATH * 2];

static HWND xctl(const wchar_t *cls, const wchar_t *txt, DWORD style, int id)
{
    return CreateWindowExW(!wcscmp(cls, WC_LISTVIEWW) ? WS_EX_CLIENTEDGE : 0, cls, txt,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, g_xw,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void x_layout(void)
{
    RECT r;
    int W, H;
    GetClientRect(g_xw, &r);
    W = r.right;
    H = r.bottom;
    MoveWindow(GetDlgItem(g_xw, IDX_LBLVOL), S(10), S(14), S(60), S(18), TRUE);
    MoveWindow(g_xvol, S(72), S(10), W - S(82), S(200), TRUE);
    MoveWindow(GetDlgItem(g_xw, IDX_UP), S(10), S(40), S(100), S(26), TRUE);
    MoveWindow(g_xwhere, S(118), S(45), W - S(128), S(18), TRUE);
    MoveWindow(g_xlist, S(10), S(72), W - S(20), H - S(72) - S(92), TRUE);
    MoveWindow(GetDlgItem(g_xw, IDX_EXTRACT), S(10), H - S(84), S(190), S(30), TRUE);
    MoveWindow(GetDlgItem(g_xw, IDX_XCANCEL), S(208), H - S(84), S(100), S(30), TRUE);
    MoveWindow(GetDlgItem(g_xw, IDX_CLOSE), W - S(110), H - S(84), S(100), S(30), TRUE);
    MoveWindow(g_xprog, S(10), H - S(46), W - S(20), S(16), TRUE);
    MoveWindow(g_xstatus, S(10), H - S(26), W - S(20), S(18), TRUE);
}

static int sys_icon(const DirEnt *e)
{
    SHFILEINFOW fi;
    wchar_t nm[260];
    int n = e->nlen < 259 ? e->nlen : 259;
    memcpy(nm, e->name, n * sizeof(wchar_t));
    nm[n] = 0;
    memset(&fi, 0, sizeof(fi));
    SHGetFileInfoW(nm, e->is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    return fi.iIcon;
}

static void x_fill(void)
{
    int i;
    wchar_t t[300];
    SendMessageW(g_xlist, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_xlist);
    for (i = 0; i < g_cur.n; i++) {
        DirEnt *e = &g_cur.e[i];
        LVITEMW it;
        char dt[24];
        int n = e->nlen < 255 ? e->nlen : 255, k;
        memcpy(t, e->name, n * sizeof(wchar_t));
        t[n] = 0;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = i;
        it.lParam = i;
        it.pszText = t;
        it.iImage = sys_icon(e);
        k = (int)SendMessageW(g_xlist, LVM_INSERTITEMW, 0, (LPARAM)&it);
        if (e->is_dir) wcscpy(t, (e->attr & FA_REPARSE) ? L"atalho" : L"pasta");
        else fmt_sizew(e->size, t);
        ListView_SetItemText(g_xlist, k, 1, t);
        ft_to_str(e->mtime, dt);
        MultiByteToWideChar(CP_ACP, 0, dt, -1, t, 300);
        ListView_SetItemText(g_xlist, k, 2, t);
    }
    SendMessageW(g_xlist, WM_SETREDRAW, TRUE, 0);
    SetWindowTextW(g_xwhere, g_xpath);
    EnableWindow(GetDlgItem(g_xw, IDX_UP), g_xdepth > 0);
    swprintf(t, 300, L"%d itens. Dê dois cliques numa pasta para abri-la; selecione itens e clique em Extrair.",
             g_cur.n);
    SetWindowTextW(g_xstatus, t);
}

static int x_load(u64 rec)
{
    DirList dl;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    int r = nt_list(&g_nt, rec, &dl);
    SetCursor(old);
    if (r != 0) {
        MessageBoxW(g_xw, L"Não foi possível ler esta pasta (imagem danificada?).", X_TITLE, MB_ICONERROR);
        return -1;
    }
    dl_free(&g_cur);
    g_cur = dl;
    return 0;
}

static void x_open_volume(int k)
{
    if (g_nt_ok) nt_close(&g_nt);
    g_nt_ok = 0;
    dl_free(&g_cur);
    ListView_DeleteAllItems(g_xlist);
    if (k < 0 || k >= g_nvols || nt_open(&g_nt, &g_vdev, g_vols[k].start, g_vols[k].len) != 0) {
        MessageBoxW(g_xw, L"Não foi possível abrir esta partição.", X_TITLE, MB_ICONERROR);
        return;
    }
    g_nt_ok = 1;
    g_xdepth = 0;
    g_xrec[0] = 5; /* pasta raiz */
    wcscpy(g_xpath, L"\\");
    g_xlen[0] = 1;
    if (x_load(5) == 0) x_fill();
}

static void x_enter(int idx)
{
    DirEnt *e;
    int n;
    if (idx < 0 || idx >= g_cur.n) return;
    e = &g_cur.e[idx];
    if (!e->is_dir) return;
    if (e->attr & FA_REPARSE) {
        MessageBoxW(g_xw, L"Este item é um atalho de pasta (junction) do Windows e aponta para outro lugar.",
                    X_TITLE, MB_ICONINFORMATION);
        return;
    }
    if (g_xdepth >= 254) return;
    n = (int)wcslen(g_xpath);
    if (n + e->nlen + 2 >= 4096) return;
    {
        u64 rec = e->rec;
        u16 nm[256];
        int nl = e->nlen;
        memcpy(nm, e->name, sizeof(nm));
        if (x_load(rec) != 0) return;
        g_xdepth++;
        g_xrec[g_xdepth] = rec;
        memcpy(g_xpath + n, nm, nl * sizeof(wchar_t));
        g_xpath[n + nl] = L'\\';
        g_xpath[n + nl + 1] = 0;
        g_xlen[g_xdepth] = n + nl + 1;
    }
    x_fill();
}

static void x_up(void)
{
    if (g_xdepth == 0) return;
    if (x_load(g_xrec[g_xdepth - 1]) != 0) return;
    g_xdepth--;
    g_xpath[g_xlen[g_xdepth]] = 0;
    x_fill();
}

static void x_set_running(int r)
{
    g_xrunning = r;
    EnableWindow(GetDlgItem(g_xw, IDX_EXTRACT), !r);
    EnableWindow(GetDlgItem(g_xw, IDX_XCANCEL), r);
    EnableWindow(GetDlgItem(g_xw, IDX_UP), !r && g_xdepth > 0);
    EnableWindow(g_xvol, !r);
    EnableWindow(g_xlist, !r);
}

static DWORD WINAPI xworker(LPVOID a)
{
    (void)a;
    if (setjmp(g_jmp) == 0) extract_entries(&g_nt, g_xsel, g_nxsel, g_xdest);
    else g_xs.errors++; /* erro grave (ex.: memoria): a imagem foi fechada */
    PostMessageW(g_xw, WM_APP_XDONE, 0, 0);
    return 0;
}

static int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data)
{
    (void)lp;
    if (msg == BFFM_INITIALIZED && ((wchar_t *)data)[0]) SendMessageW(h, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void x_extract(void)
{
    int i, n = ListView_GetSelectedCount(g_xlist);
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    if (!g_nt_ok) return;
    free(g_xsel);
    g_xsel = NULL;
    g_nxsel = 0;
    if (n == 0) {
        if (MessageBoxW(g_xw, L"Nenhum item selecionado.\n\nExtrair a pasta atual inteira?", X_TITLE,
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        g_xsel = calloc(1, sizeof(DirEnt));
        if (!g_xsel) return;
        g_xsel[0].rec = g_xrec[g_xdepth];
        g_xsel[0].is_dir = 1;
        if (g_xdepth == 0) {
            const wchar_t *nm = L"Particao";
            for (i = 0; nm[i]; i++) g_xsel[0].name[i] = nm[i];
            g_xsel[0].nlen = i;
        } else { /* nome da pasta atual = ultimo componente do caminho */
            int end = g_xlen[g_xdepth] - 1, beg = g_xlen[g_xdepth - 1];
            for (i = beg; i < end && i - beg < 255; i++) g_xsel[0].name[i - beg] = g_xpath[i];
            g_xsel[0].nlen = i - beg;
        }
        g_nxsel = 1;
    } else {
        int k = -1;
        g_xsel = malloc(sizeof(DirEnt) * n);
        if (!g_xsel) return;
        while ((k = ListView_GetNextItem(g_xlist, k, LVNI_SELECTED)) >= 0 && g_nxsel < n) {
            LVITEMW it;
            memset(&it, 0, sizeof(it));
            it.mask = LVIF_PARAM;
            it.iItem = k;
            SendMessageW(g_xlist, LVM_GETITEMW, 0, (LPARAM)&it);
            g_xsel[g_nxsel++] = g_cur.e[it.lParam];
        }
    }
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_xw;
    bi.lpszTitle = L"Escolha a pasta onde salvar os arquivos recuperados:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | 0x0040 /* BIF_NEWDIALOGSTYLE: botao "Nova pasta" */;
    bi.lpfn = browse_cb;
    bi.lParam = (LPARAM)g_xdest;
    pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    if (!SHGetPathFromIDListW(pidl, g_xdest)) { CoTaskMemFree(pidl); return; }
    CoTaskMemFree(pidl);

    g_cancel = 0;
    g_notify = g_xw;
    SendMessageW(g_xprog, PBM_SETPOS, 0, 0);
    SetWindowTextW(g_xstatus, L"Calculando o tamanho...");
    SetWindowTextW(g_log, L"");
    x_set_running(1);
    {
        DWORD tid;
        HANDLE th = CreateThread(NULL, 0, xworker, NULL, 0, &tid);
        if (!th) { x_set_running(0); g_notify = NULL; return; }
        CloseHandle(th);
    }
}

static LRESULT CALLBACK xproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        LVCOLUMNW c;
        SHFILEINFOW fi;
        HIMAGELIST il;
        int i;
        static const wchar_t *cols[] = {L"Nome", L"Tamanho", L"Modificado"};
        static const int widths[] = {360, 100, 130};
        g_xw = h;
        xctl(L"STATIC", L"Partição:", SS_LEFT, IDX_LBLVOL);
        g_xvol = xctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDX_VOL);
        xctl(L"BUTTON", L"Pasta acima", BS_PUSHBUTTON | WS_TABSTOP, IDX_UP);
        g_xwhere = xctl(L"STATIC", L"\\", SS_LEFT | SS_PATHELLIPSIS, IDX_WHERE);
        g_xlist = xctl(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS | WS_TABSTOP, IDX_LIST);
        xctl(L"BUTTON", L"Extrair selecionados...", BS_DEFPUSHBUTTON | WS_TABSTOP, IDX_EXTRACT);
        xctl(L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, IDX_XCANCEL);
        xctl(L"BUTTON", L"Fechar", BS_PUSHBUTTON | WS_TABSTOP, IDX_CLOSE);
        g_xprog = xctl(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDX_XPROG);
        g_xstatus = xctl(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, IDX_XSTATUS);
        SendMessageW(g_xprog, PBM_SETRANGE32, 0, 1000);
        SendMessageW(g_xlist, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT);
        for (i = 0; i < 3; i++) {
            memset(&c, 0, sizeof(c));
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.fmt = i == 1 ? LVCFMT_RIGHT : LVCFMT_LEFT;
            c.pszText = (wchar_t *)cols[i];
            c.cx = S(widths[i]);
            SendMessageW(g_xlist, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
        }
        memset(&fi, 0, sizeof(fi));
        il = (HIMAGELIST)SHGetFileInfoW(L"x.txt", FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                                        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
        if (il) ListView_SetImageList(g_xlist, il, LVSIL_SMALL);
        for (i = 0; i < g_nvols; i++) {
            wchar_t t[200], sz[32], lb[100];
            fmt_sizew(g_vols[i].len, sz);
            MultiByteToWideChar(CP_OEMCP, 0, g_vols[i].label, -1, lb, 100);
            swprintf(t, 200, L"%d:  NTFS  %ls   %ls%ls%ls", i + 1, sz, lb[0] ? L"“" : L"", lb,
                     lb[0] ? L"”" : L"");
            SendMessageW(g_xvol, CB_ADDSTRING, 0, (LPARAM)t);
        }
        EnumChildWindows(h, set_font_cb, (LPARAM)g_font);
        SendMessageW(GetDlgItem(h, IDX_EXTRACT), WM_SETFONT, (WPARAM)g_bold, TRUE);
        x_layout();
        /* abre a maior particao (normalmente a do Windows) */
        {
            int best = 0;
            for (i = 1; i < g_nvols; i++)
                if (g_vols[i].len > g_vols[best].len) best = i;
            SendMessageW(g_xvol, CB_SETCURSEL, best, 0);
            x_open_volume(best);
        }
        SetFocus(g_xlist);
        return 0;
    }
    case WM_SIZE:
        x_layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(480);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(360);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDX_VOL:
            if (HIWORD(wp) == CBN_SELCHANGE) x_open_volume((int)SendMessageW(g_xvol, CB_GETCURSEL, 0, 0));
            break;
        case IDX_UP: x_up(); break;
        case IDOK: /* Enter na lista: abre a pasta */
            if (GetFocus() == g_xlist && !g_xrunning) {
                int k = ListView_GetNextItem(g_xlist, -1, LVNI_FOCUSED);
                if (k >= 0) {
                    LVITEMW it;
                    memset(&it, 0, sizeof(it));
                    it.mask = LVIF_PARAM;
                    it.iItem = k;
                    SendMessageW(g_xlist, LVM_GETITEMW, 0, (LPARAM)&it);
                    x_enter((int)it.lParam);
                }
            }
            break;
        case IDX_EXTRACT: x_extract(); break;
        case IDX_XCANCEL:
            g_cancel = 1;
            SetWindowTextW(g_xstatus, L"Cancelando...");
            break;
        case IDX_CLOSE: SendMessageW(h, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDX_LIST && nh->code == NM_DBLCLK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
            if (ia->iItem >= 0) {
                LVITEMW it;
                memset(&it, 0, sizeof(it));
                it.mask = LVIF_PARAM;
                it.iItem = ia->iItem;
                SendMessageW(g_xlist, LVM_GETITEMW, 0, (LPARAM)&it);
                x_enter((int)it.lParam);
            }
        } else if (nh->idFrom == IDX_LIST && nh->code == LVN_KEYDOWN && !g_xrunning) {
            NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
            if (kd->wVKey == VK_BACK) x_up();
            else if (kd->wVKey == VK_RETURN) {
                int k = ListView_GetNextItem(g_xlist, -1, LVNI_FOCUSED);
                if (k >= 0) {
                    LVITEMW it;
                    memset(&it, 0, sizeof(it));
                    it.mask = LVIF_PARAM;
                    it.iItem = k;
                    SendMessageW(g_xlist, LVM_GETITEMW, 0, (LPARAM)&it);
                    x_enter((int)it.lParam);
                }
            }
        }
        return 0;
    }
    case WM_APP_PROG: {
        Prog *p = (Prog *)lp;
        wchar_t t[200];
        int pm = p->total ? (int)((double)p->done * 1000.0 / (double)p->total) : 0;
        SendMessageW(g_xprog, PBM_SETPOS, pm, 0);
        swprintf(t, 200, L"Extraindo...  %.1f%%   %llu de %llu MB   %.1f MB/s   restante %02lu:%02lu:%02lu",
                 pm / 10.0, (unsigned long long)(p->done >> 20), (unsigned long long)(p->total >> 20), p->speed,
                 p->eta / 3600, (p->eta / 60) % 60, p->eta % 60);
        SetWindowTextW(g_xstatus, t);
        free(p);
        return 0;
    }
    case WM_APP_XDONE: {
        wchar_t t[700], sz[32];
        g_notify = NULL;
        x_set_running(0);
        SendMessageW(g_xprog, PBM_SETPOS, g_xs.cancelled ? 0 : 1000, 0);
        fmt_sizew(g_xs.bytes, sz);
        swprintf(t, 700, L"%ls\n\n%llu arquivo(s) e %llu pasta(s), %ls, salvos em:\n%ls%ls%ls",
                 g_xs.cancelled ? L"Extração cancelada." : L"Extração concluída!",
                 (unsigned long long)g_xs.files, (unsigned long long)g_xs.dirs, sz, g_xdest,
                 g_xs.skipped ? L"\n\nAlguns itens foram ignorados (arquivos comprimidos/criptografados pelo NTFS ou "
                                L"atalhos de pasta). Veja a lista no registro da janela principal."
                              : L"",
                 g_xs.errors ? L"\n\nATENÇÃO: alguns arquivos não puderam ser extraídos. Veja o registro da "
                               L"janela principal."
                             : L"");
        SetWindowTextW(g_xstatus, g_xs.cancelled ? L"Cancelado." : L"Concluído.");
        if (!g_xclose_after)
            MessageBoxW(h, t, X_TITLE, (g_xs.errors ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_OK);
        else {
            EnableWindow(g_hwnd, TRUE);
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_xrunning) {
            if (MessageBoxW(h, L"Uma extração está em andamento. Cancelar e fechar?", X_TITLE,
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                g_xclose_after = 1;
                g_cancel = 1;
            }
            return 0;
        }
        EnableWindow(g_hwnd, TRUE); /* antes de destruir: o foco volta para a janela principal */
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_nt_ok) nt_close(&g_nt);
        g_nt_ok = 0;
        dl_free(&g_cur);
        free(g_xsel);
        g_xsel = NULL;
        imgsrc_close();
        g_xw = NULL;
        g_xclose_after = 0;
        EnableWindow(g_hwnd, TRUE);
        SetForegroundWindow(g_hwnd);
        SetWindowTextW(g_status, L"Pronto.");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void open_explorer(void)
{
    static int registered;
    RECT r;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = xproc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"hdcloneExplorer";
        RegisterClassW(&wc);
        registered = 1;
    }
    r.left = 0; r.top = 0; r.right = S(680); r.bottom = S(520);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    EnableWindow(g_hwnd, FALSE);
    CreateWindowW(L"hdcloneExplorer", X_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                  r.right - r.left, r.bottom - r.top, g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_xw) { EnableWindow(g_hwnd, TRUE); imgsrc_close(); return; }
    ShowWindow(g_xw, SW_SHOW);
    UpdateWindow(g_xw);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    NONCLIENTMETRICSW ncm;
    WNDCLASSW wc;
    RECT r;
    MSG m;
    HDC dc;
    (void)prev; (void)cmd;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* Windows 64 bits: abre a versao 64 bits (necessaria para o VSS), se existir */
    if (relaunch_64("hdclone-gui64.exe", 0, NULL) == 0) return 0;
    /* COM na thread da janela + seguranca COM exigida pelo VSS (antes de qualquer dialogo) */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    vss_security();

    dc = GetDC(NULL);
    g_scale = GetDeviceCaps(dc, LOGPIXELSY) / 96.0;
    ReleaseDC(NULL, dc);
    if (g_scale < 1.0) g_scale = 1.0;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm); /* compilado com _WIN32_WINNT=0x0501: tamanho aceito pelo XP */
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_bold = CreateFontIndirectW(&ncm.lfMessageFont);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"hdcloneGui";
    RegisterClassW(&wc);

    r.left = 0; r.top = 0; r.right = S(640); r.bottom = S(650);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    CreateWindowW(L"hdcloneGui", APP_TITLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                  CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!(g_xw && IsDialogMessageW(g_xw, &m)) && !IsDialogMessageW(g_hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
