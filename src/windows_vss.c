/* hdclone - volumes do Windows, listagem de discos, VSS */

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
