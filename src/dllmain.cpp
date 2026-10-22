// Entry point. The DLL can be loaded either as dinput8.dll (proxy, exports forwarded to the system
// dinput8) or as an .asi plugin (OptiScaler's LoadAsiPlugins, Ultimate ASI Loader, ...).
#include "common.h"

static HMODULE s_self;
static HMODULE s_realDinput8;

static FARPROC RealDinput8(const char* name)
{
    if (!s_realDinput8)
    {
        wchar_t p[MAX_PATH];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 20)
            return nullptr;
        lstrcatW(p, L"\\dinput8.dll");
        s_realDinput8 = LoadLibraryW(p);
    }
    return s_realDinput8 ? GetProcAddress(s_realDinput8, name) : nullptr;
}

void OverlayHookDirectInput(void* di);
extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    typedef HRESULT(WINAPI * F)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    F f = (F)RealDinput8("DirectInput8Create");
    HRESULT hr = f ? f(h, ver, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out)
        OverlayHookDirectInput(*out);
    return hr;
}
extern "C" HRESULT WINAPI Proxy_DllCanUnloadNow()
{
    typedef HRESULT(WINAPI * F)();
    F f = (F)RealDinput8("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
extern "C" HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* p)
{
    typedef HRESULT(WINAPI * F)(REFCLSID, REFIID, LPVOID*);
    F f = (F)RealDinput8("DllGetClassObject");
    return f ? f(c, r, p) : CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI Proxy_DllRegisterServer()
{
    typedef HRESULT(WINAPI * F)();
    F f = (F)RealDinput8("DllRegisterServer");
    return f ? f() : E_FAIL;
}
extern "C" HRESULT WINAPI Proxy_DllUnregisterServer()
{
    typedef HRESULT(WINAPI * F)();
    F f = (F)RealDinput8("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
extern "C" void* WINAPI Proxy_GetdfDIJoystick()
{
    typedef void*(WINAPI * F)();
    F f = (F)RealDinput8("GetdfDIJoystick");
    return f ? f() : nullptr;
}

// Lets tools detect the mod.
extern "C" const char* WINAPI GRB_DLSS_Version()
{
    return MOD_VERSION;
}

static DWORD WINAPI BootThread(LPVOID)
{
    ModBootstrap();
    return 0;
}

extern "C" BOOL WINAPI DllMainCRTStartup(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        s_self = hinst;
        DisableThreadLibraryCalls(hinst);

        wchar_t self[MAX_PATH];
        GetModuleFileNameW(hinst, self, MAX_PATH);
        lstrcpyW(g_modDir, self);
        wchar_t* sl = wcsrchr(g_modDir, L'\\');
        if (sl)
            sl[1] = 0;

        LoadConfig();
        wchar_t logp[MAX_PATH];
        lstrcpyW(logp, g_modDir);
        lstrcatW(logp, L"GRB_DLSS.log");
        LogInit(logp);

        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char a[MAX_PATH * 2], b[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, exe, -1, a, sizeof(a), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, self, -1, b, sizeof(b), nullptr, nullptr);
        LOGI("%s %s loaded as %s into %s", MOD_NAME, MOD_VERSION, b, a);
        LOGI("Config: Enabled=%d ToggleKey=0x%X Preset=%d QualityMode=%d JitterScale=(%.1f, %.1f) DepthInverted=%d "
             "ResponsiveMask=%d DebugView=%d",
             g_cfg.enabled, g_cfg.toggleKey, g_cfg.preset, g_cfg.qualityOverride, g_cfg.jitterSignX, g_cfg.jitterSignY,
             g_cfg.depthInverted, g_cfg.useBiasMask, g_cfg.debugView);

        if (g_cfg.enabled)
        {
            HANDLE t = CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
            if (t)
                CloseHandle(t);
        }
        else
            LOGI("Disabled in GRB_DLSS.ini");
    }
    return TRUE;
}
