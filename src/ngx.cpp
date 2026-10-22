#include "ngx.h"

NgxApi g_ngx;

static void NgxLogCallback(const char* message, int level, int component)
{
    if (!message)
        return;
    // strip trailing newline
    static char buf[1024];
    size_t n = strlen(message);
    if (n >= sizeof(buf))
        n = sizeof(buf) - 1;
    memcpy(buf, message, n);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        n--;
    buf[n] = 0;
    Log(LOG_DBG, "[NGX] %s", buf);
}

static bool FileExists(const wchar_t* p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static HMODULE TryLoad(const wchar_t* path)
{
    HMODULE m = LoadLibraryW(path);
    char a[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof(a), nullptr, nullptr);
    LOGI("NGX: LoadLibrary(%s) -> %p (err %lu)", a, m, m ? 0 : GetLastError());
    return m;
}

bool NgxLoad()
{
    if (g_ngx.module)
        return true;

    HMODULE m = nullptr;

    // 1) An NGX implementation placed beside the game (e.g. OptiScaler installed as nvngx.dll).
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_modDir);
    lstrcatW(p, L"nvngx.dll");
    if (FileExists(p))
        m = TryLoad(p);

    // 2) The driver's NGX core. If OptiScaler is loaded as a proxy (dxgi.dll, winmm.dll, ...), it hooks
    //    LoadLibrary and hands us its own module here instead - that is how it receives our DLSS calls.
    if (!m)
    {
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0, KEY_READ, &key) ==
            ERROR_SUCCESS)
        {
            wchar_t dir[MAX_PATH];
            DWORD type = 0, size = sizeof(dir);
            if (RegQueryValueExW(key, L"FullPath", nullptr, &type, (BYTE*)dir, &size) == ERROR_SUCCESS &&
                (type == REG_SZ || type == REG_EXPAND_SZ))
            {
                dir[MAX_PATH - 1] = 0;
                size_t l = wcslen(dir);
                if (l && dir[l - 1] != L'\\')
                    lstrcatW(dir, L"\\");
                lstrcatW(dir, L"_nvngx.dll");
                m = TryLoad(dir);
            }
            RegCloseKey(key);
        }
    }
    if (!m)
        m = TryLoad(L"_nvngx.dll");
    if (!m)
    {
        LOGE("NGX: could not load the NVIDIA NGX core (_nvngx.dll). Is this an NVIDIA RTX GPU with a current driver?");
        return false;
    }

    wchar_t mp[MAX_PATH];
    GetModuleFileNameW(m, mp, MAX_PATH);
    char a[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, mp, -1, a, sizeof(a), nullptr, nullptr);
    const wchar_t* fn = wcsrchr(mp, L'\\');
    fn = fn ? fn + 1 : mp;
    g_ngx.isOptiScaler = lstrcmpiW(fn, L"_nvngx.dll") != 0 || GetProcAddress(m, "OptiScalerVersion") != nullptr;
    LOGI("NGX: provider module = %s%s", a, g_ngx.isOptiScaler ? "  (not the driver core -> OptiScaler / external NGX)" : "");

    g_ngx.module = m;
    g_ngx.Init_Ext = (PFN_D3D11_Init_Ext)GetProcAddress(m, "NVSDK_NGX_D3D11_Init_Ext");
    g_ngx.Init_ProjectID = (PFN_D3D11_Init_ProjectID)GetProcAddress(m, "NVSDK_NGX_D3D11_Init_ProjectID");
    g_ngx.Shutdown1 = (PFN_D3D11_Shutdown1)GetProcAddress(m, "NVSDK_NGX_D3D11_Shutdown1");
    g_ngx.GetCapabilityParameters =
        (PFN_D3D11_GetCapabilityParameters)GetProcAddress(m, "NVSDK_NGX_D3D11_GetCapabilityParameters");
    g_ngx.AllocateParameters = (PFN_D3D11_AllocateParameters)GetProcAddress(m, "NVSDK_NGX_D3D11_AllocateParameters");
    g_ngx.DestroyParameters = (PFN_D3D11_DestroyParameters)GetProcAddress(m, "NVSDK_NGX_D3D11_DestroyParameters");
    g_ngx.CreateFeature = (PFN_D3D11_CreateFeature)GetProcAddress(m, "NVSDK_NGX_D3D11_CreateFeature");
    g_ngx.ReleaseFeature = (PFN_D3D11_ReleaseFeature)GetProcAddress(m, "NVSDK_NGX_D3D11_ReleaseFeature");
    g_ngx.EvaluateFeature = (PFN_D3D11_EvaluateFeature)GetProcAddress(m, "NVSDK_NGX_D3D11_EvaluateFeature");

    if ((!g_ngx.Init_Ext && !g_ngx.Init_ProjectID) || !g_ngx.AllocateParameters || !g_ngx.CreateFeature ||
        !g_ngx.EvaluateFeature || !g_ngx.ReleaseFeature)
    {
        LOGE("NGX: required D3D11 exports missing (Init_Ext=%p Init_ProjectID=%p Alloc=%p Create=%p Eval=%p)",
             g_ngx.Init_Ext, g_ngx.Init_ProjectID, g_ngx.AllocateParameters, g_ngx.CreateFeature, g_ngx.EvaluateFeature);
        g_ngx.module = nullptr;
        return false;
    }
    return true;
}

static const wchar_t* s_paths[2];
static wchar_t s_exeDir[MAX_PATH];

bool NgxInit(ID3D11Device* dev)
{
    if (!NgxLoad())
        return false;

    GetModuleFileNameW(nullptr, s_exeDir, MAX_PATH);
    wchar_t* sl = wcsrchr(s_exeDir, L'\\');
    if (sl)
        sl[1] = 0;
    s_paths[0] = g_modDir;
    s_paths[1] = s_exeDir;

    NVSDK_NGX_FeatureCommonInfo info;
    memset(&info, 0, sizeof(info));
    info.PathListInfo.Path = s_paths;
    info.PathListInfo.Length = 2;
    if (g_cfg.ngxLogging)
    {
        info.LoggingInfo.LoggingCallback = NgxLogCallback;
        info.LoggingInfo.MinimumLoggingLevel = g_cfg.ngxLogging >= 2 ? 2 : 1;
        info.LoggingInfo.DisableOtherLoggingSinks = false;
    }

    NVSDK_NGX_Result r = 0xBAD00000u;
    if (g_ngx.Init_ProjectID)
    {
        r = g_ngx.Init_ProjectID("8a5f1c3e-6b2d-4e9a-9c71-0d4b2f6a1e57", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_modDir, dev,
                                 NVSDK_NGX_Version_API, &info);
        LOGI("NGX: D3D11_Init_ProjectID -> 0x%08X", r);
    }
    if (!NGX_OK(r) && g_ngx.Init_Ext)
    {
        r = g_ngx.Init_Ext(0x1337ull, g_modDir, dev, NVSDK_NGX_Version_API, &info);
        LOGI("NGX: D3D11_Init_Ext -> 0x%08X", r);
    }
    if (!NGX_OK(r))
    {
        LOGE("NGX: initialisation failed (0x%08X)", r);
        return false;
    }
    return true;
}

bool NgxDlssAvailable(ID3D11Device* dev)
{
    if (!g_ngx.GetCapabilityParameters)
        return true; // cannot check, try anyway
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Result r = g_ngx.GetCapabilityParameters(&caps);
    if (!NGX_OK(r) || !caps)
    {
        LOGI("NGX: GetCapabilityParameters -> 0x%08X (continuing)", r);
        return true;
    }
    int avail = -1, needsDriver = 0;
    unsigned minMaj = 0, minMin = 0;
    caps->GetI("SuperSampling.Available", &avail);
    caps->GetI("SuperSampling.NeedsUpdatedDriver", &needsDriver);
    caps->GetUI("SuperSampling.MinDriverVersionMajor", &minMaj);
    caps->GetUI("SuperSampling.MinDriverVersionMinor", &minMin);
    int initRes = 0;
    caps->GetI("SuperSampling.FeatureInitResult", &initRes);
    LOGI("NGX: SuperSampling.Available=%d NeedsUpdatedDriver=%d (min %u.%u) FeatureInitResult=0x%08X", avail, needsDriver,
         minMaj, minMin, (unsigned)initRes);
    if (g_ngx.DestroyParameters)
        g_ngx.DestroyParameters(caps);
    return avail != 0;
}
