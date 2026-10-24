// In-game settings menu (Home key by default): render resolution in exact percent, DLSS preset, DLSS on/off.
// Drawn with D3D11 into the game's back buffer from a hook on the swap chain the game presents with
// (captured at creation, so it is OptiScaler's wrapper when OptiScaler is installed).
#include "common.h"
#include "font.h"
#include <dxgi1_2.h>

// ---------------------------------------------------------------- mod.cpp / renderscale.cpp API
float ModGetRenderScale();          // 0 = game decides
void ModSetRenderScale(float s);    // 0 = game decides
bool ModGetDlssOn();
void ModSetDlssOn(bool on);
void ModGetStatus(UINT* lowW, UINT* lowH, UINT* fullW, UINT* fullH, bool* dlssActive, bool* taaSeen);
void ModSetOverlayThread(bool on);
float RenderScaleGameRatio();
bool ChainExport(HMODULE mod, const char* name, void* hook, void** orig);

// ---------------------------------------------------------------- vtable patching with per-vtable originals
struct VtRec
{
    void** vt;
    void* orig[32];
    void* genuine[32]; // the system implementation (same module as the vtable), if we saw it
};

static HMODULE ModuleOf(const void* p)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)p, &m))
        return nullptr;
    return m;
}
static VtRec s_vts[24];
static LONG s_vtCount;
static SRWLOCK s_vtLock = SRWLOCK_INIT;

static void* OrigFor(void* obj, int idx)
{
    void** vt = *(void***)obj;
    LONG n = s_vtCount;
    for (LONG i = 0; i < n; i++)
        if (s_vts[i].vt == vt && s_vts[i].orig[idx])
            return s_vts[i].orig[idx];
    // Called through a vtable we did not patch: another layer copied a vtable after we had hooked it
    // (OptiScaler / driver wrappers do). The original is then the one we saved for the vtable it was
    // copied from - use the most recent original we have for this slot instead of calling null.
    static volatile LONG s_warned;
    if (InterlockedIncrement(&s_warned) <= 5)
        LOGI("Hook: object %p calls slot %d through vtable %p we did not patch - using saved original", obj, idx, vt);
    for (LONG i = n - 1; i >= 0; i--)
        if (s_vts[i].orig[idx])
            return s_vts[i].orig[idx];
    return nullptr;
}

static void* GenuineFor(void* obj, int idx)
{
    void** vt = *(void***)obj;
    LONG n = s_vtCount;
    for (LONG i = 0; i < n; i++)
        if (s_vts[i].vt == vt && s_vts[i].genuine[idx])
            return s_vts[i].genuine[idx];
    return nullptr;
}

// Per-thread nesting depth of our swap chain creation hooks. Another hook (e.g. the Ubisoft Connect
// overlay) can end up forwarding to us while we forward to it - the call then ping-pongs until the
// stack overflows. When we are re-entered on the same thread we call the system implementation
// directly, which ends any such loop (a legitimately nested call reaches the same function anyway).
static DWORD s_depthTls = TLS_OUT_OF_INDEXES;
static LONG EnterDepth()
{
    if (s_depthTls == TLS_OUT_OF_INDEXES)
        return 1;
    LONG d = (LONG)(intptr_t)TlsGetValue(s_depthTls) + 1;
    TlsSetValue(s_depthTls, (void*)(intptr_t)d);
    return d;
}
static void LeaveDepth()
{
    if (s_depthTls == TLS_OUT_OF_INDEXES)
        return;
    LONG d = (LONG)(intptr_t)TlsGetValue(s_depthTls) - 1;
    TlsSetValue(s_depthTls, (void*)(intptr_t)(d < 0 ? 0 : d));
}

static bool HookSlot(void* obj, int idx, void* hook)
{
    void** vt = *(void***)obj;
    AcquireSRWLockExclusive(&s_vtLock);
    VtRec* r = nullptr;
    for (LONG i = 0; i < s_vtCount; i++)
        if (s_vts[i].vt == vt)
            r = &s_vts[i];
    if (!r)
    {
        if (s_vtCount >= 24)
        {
            ReleaseSRWLockExclusive(&s_vtLock);
            return false;
        }
        r = &s_vts[s_vtCount];
        memset(r, 0, sizeof(*r));
        r->vt = vt;
        MemoryBarrier();
        s_vtCount++;
    }
    bool ok = true;
    // see PatchSlot in mod.cpp: never re-hook a slot another layer has hooked on top of us
    if (vt[idx] != hook && !(r->orig[idx] && vt[idx] != r->orig[idx]))
    {
        DWORD old;
        if (VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old) ||
            VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
        {
            r->orig[idx] = vt[idx];
            HMODULE vm = ModuleOf(vt);
            if (!r->genuine[idx] && vm && ModuleOf(vt[idx]) == vm)
                r->genuine[idx] = vt[idx];
            MemoryBarrier();
            vt[idx] = hook;
            DWORD tmp;
            VirtualProtect(&vt[idx], sizeof(void*), old, &tmp);
        }
        else
            ok = false;
    }
    ReleaseSRWLockExclusive(&s_vtLock);
    return ok;
}

// ---------------------------------------------------------------- menu state
static volatile bool s_open;
static HWND s_hwnd; // window of the swap chain the game presents with
static uint64_t s_toastUntil;
static char s_toast[96];
static int s_sel;
static char s_edit[8];
static int s_editLen; // >0 while typing a percentage
static bool s_dirty;  // unsaved changes

static uint64_t QpcAfterMs(double ms)
{
    return NowQpc() + (uint64_t)(ms / QpcToMs(1));
}

void OverlayToast(const char* text)
{
    lstrcpynA(s_toast, text, sizeof(s_toast));
    s_toastUntil = QpcAfterMs(2500);
}

bool OverlayIsOpen()
{
    return s_open;
}

// ---------------------------------------------------------------- input blocking while the menu is open
// Every window we subclass keeps its own original procedure. (A second swap chain on a second window -
// e.g. created by NVIDIA Smooth Motion - must not replace the game window's procedure: the game window
// would then stop running its own message handling, including the WM_SETCURSOR that hides the cursor.)
struct Subclassed
{
    HWND hwnd;
    WNDPROC old;
};
static Subclassed s_sub[8];
static volatile LONG s_subCount;

static WNDPROC OldProcFor(HWND h)
{
    LONG n = s_subCount;
    for (LONG i = 0; i < n; i++)
        if (s_sub[i].hwnd == h)
            return s_sub[i].old;
    return nullptr;
}

static LRESULT CALLBACK MenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC old = OldProcFor(h);
    if (s_open && h == s_hwnd)
    {
        switch (m)
        {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEWHEEL:
            return 0;
        case WM_INPUT:
            return DefWindowProcW(h, m, w, l);
        }
    }
    return old ? CallWindowProcW(old, h, m, w, l) : DefWindowProcW(h, m, w, l);
}

static bool OwnWindow(HWND h)
{
    DWORD pid = 0;
    return h && GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId();
}

static void SubclassWindow(HWND h)
{
    if (!h || !OwnWindow(h) || OldProcFor(h) || s_subCount >= 8)
        return;
    LONG i = s_subCount;
    s_sub[i].hwnd = h;
    s_sub[i].old = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC); // set before the swap so messages never miss it
    InterlockedIncrement(&s_subCount);
    WNDPROC old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)MenuWndProc);
    if (old)
    {
        s_sub[i].old = old;
        LOGI("Menu: window %p subclassed for input blocking", h);
    }
}

// DirectInput (we are the dinput8 proxy): blank keyboard/mouse state while the menu is open
struct GUID_ { uint32_t a; uint16_t b, c; uint8_t d[8]; };
static const GUID_ kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID_ kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static void* s_diBlocked[8];
static LONG s_diBlockedCount;

static bool DiBlocked(void* dev)
{
    for (LONG i = 0; i < s_diBlockedCount; i++)
        if (s_diBlocked[i] == dev)
            return true;
    return false;
}
typedef HRESULT(STDMETHODCALLTYPE* PFN_DiGetState)(void*, DWORD, void*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DiGetData)(void*, DWORD, void*, DWORD*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DiCreateDevice)(void*, const GUID_&, void**, IUnknown*);

static HRESULT STDMETHODCALLTYPE Hk_DiGetState(void* dev, DWORD cb, void* data)
{
    HRESULT hr = ((PFN_DiGetState)OrigFor(dev, 9))(dev, cb, data);
    if (s_open && SUCCEEDED(hr) && data && DiBlocked(dev))
        memset(data, 0, cb);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_DiGetData(void* dev, DWORD cbObj, void* rg, DWORD* inOut, DWORD flags)
{
    HRESULT hr = ((PFN_DiGetData)OrigFor(dev, 10))(dev, cbObj, rg, inOut, flags);
    if (s_open && SUCCEEDED(hr) && inOut && DiBlocked(dev))
        *inOut = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_DiCreateDevice(void* di, const GUID_& g, void** out, IUnknown* outer)
{
    HRESULT hr = ((PFN_DiCreateDevice)OrigFor(di, 3))(di, g, out, outer);
    if (SUCCEEDED(hr) && out && *out)
    {
        bool kb = memcmp(&g, &kSysKeyboard, sizeof(g)) == 0, mouse = memcmp(&g, &kSysMouse, sizeof(g)) == 0;
        if ((kb || mouse) && s_diBlockedCount < 8)
        {
            s_diBlocked[s_diBlockedCount++] = *out;
            HookSlot(*out, 9, (void*)Hk_DiGetState);
            HookSlot(*out, 10, (void*)Hk_DiGetData);
            LOGI("Menu: DirectInput %s device %p hooked", kb ? "keyboard" : "mouse", *out);
        }
    }
    return hr;
}
void OverlayHookDirectInput(void* di)
{
    if (di)
        HookSlot(di, 3, (void*)Hk_DiCreateDevice);
}

// ---------------------------------------------------------------- keyboard polling
static bool s_keyPrev[256];
static uint64_t s_keyRepeat[256];
static bool Foreground()
{
    // any window of the game process (the game window, or a window an overlay / Smooth Motion added)
    return OwnWindow(GetForegroundWindow());
}
// edge-triggered, with auto-repeat when 'repeat'
static bool Pressed(int vk, bool repeat = false)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool prev = s_keyPrev[vk & 255];
    s_keyPrev[vk & 255] = down;
    if (down && !prev)
    {
        s_keyRepeat[vk & 255] = QpcAfterMs(380);
        return true;
    }
    if (down && repeat && NowQpc() > s_keyRepeat[vk & 255])
    {
        s_keyRepeat[vk & 255] = QpcAfterMs(45);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- menu logic
static const int kPresets[] = {0, 5, 6, 10, 11, 12, 13}; // default, E, F, J, K, L, M
static const int kNumPresets = 7;
static const char* PresetName(int p)
{
    static char b[4];
    if (p <= 0)
        return "Default";
    b[0] = (char)('A' + p - 1);
    b[1] = 0;
    return b;
}
static const char* QualityName(float r)
{
    if (r >= 0.95f)
        return "DLAA";
    if (r >= 0.72f)
        return "Ultra Quality";
    if (r >= 0.62f)
        return "Quality";
    if (r >= 0.54f)
        return "Balanced";
    if (r >= 0.42f)
        return "Performance";
    return "Ultra Performance";
}

enum
{
    kItemScale,
    kItemPreset,
    kItemDlss,
    kItemSave,
    kItemCount
};

static void SaveIni()
{
    wchar_t ini[MAX_PATH], v[32];
    lstrcpyW(ini, g_modDir);
    lstrcatW(ini, L"GRB_DLSS.ini");
    float s = ModGetRenderScale();
    if (s > 0.0f)
        wsprintfW(v, L"%d", (int)(s * 100.0f + 0.5f));
    else
        lstrcpyW(v, L"0");
    WritePrivateProfileStringW(L"DLSS", L"RenderScale", v, ini);
    int p = g_cfg.preset;
    if (p >= 1 && p <= 15)
    {
        v[0] = (wchar_t)(L'A' + p - 1);
        v[1] = 0;
    }
    else
        lstrcpyW(v, L"0");
    WritePrivateProfileStringW(L"DLSS", L"Preset", v, ini);
    s_dirty = false;
    LOGI("Menu: settings saved to GRB_DLSS.ini (RenderScale=%d%%, Preset=%s)", (int)(s * 100.0f + 0.5f), PresetName(p));
}

static void SetScalePercent(int pct)
{
    if (pct < 25)
        pct = 25;
    if (pct > 100)
        pct = 100;
    ModSetRenderScale(pct / 100.0f);
    s_dirty = true;
}

static int CurrentPercent()
{
    float s = ModGetRenderScale();
    if (s <= 0.0f)
        s = RenderScaleGameRatio();
    if (s <= 0.0f)
        s = 0.667f;
    return (int)(s * 100.0f + 0.5f);
}

static void HandleInput(int menuKey)
{
    bool fg = Foreground();
    if (!fg)
    {
        // keep edge state in sync so nothing fires on return
        if (menuKey)
            Pressed(menuKey);
        return;
    }
    if (menuKey && Pressed(menuKey))
    {
        s_open = !s_open;
        s_editLen = 0;
        if (!s_open && s_dirty)
            SaveIni();
        LOGI("Menu %s", s_open ? "opened" : "closed");
    }
    if (!s_open)
        return;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (Pressed(VK_ESCAPE))
    {
        if (s_editLen)
            s_editLen = 0;
        else
        {
            s_open = false;
            if (s_dirty)
                SaveIni();
        }
        return;
    }
    if (Pressed(VK_UP, true))
    {
        s_sel = (s_sel + kItemCount - 1) % kItemCount;
        s_editLen = 0;
    }
    if (Pressed(VK_DOWN, true))
    {
        s_sel = (s_sel + 1) % kItemCount;
        s_editLen = 0;
    }
    int dir = 0;
    if (Pressed(VK_LEFT, true))
        dir = -1;
    if (Pressed(VK_RIGHT, true))
        dir = 1;
    bool enter = Pressed(VK_RETURN);
    // digits (top row and numpad) type an exact percentage
    for (int d = 0; d <= 9; d++)
    {
        bool hit = Pressed('0' + d);
        hit = Pressed(VK_NUMPAD0 + d) || hit;
        if (hit && s_editLen < 3)
        {
            s_sel = kItemScale;
            s_edit[s_editLen++] = (char)('0' + d);
            s_edit[s_editLen] = 0;
        }
    }
    if (Pressed(VK_BACK) || Pressed(VK_DELETE))
    {
        if (s_editLen)
            s_edit[--s_editLen] = 0;
        else if (s_sel == kItemScale)
        {
            ModSetRenderScale(0.0f); // back to the game's own resolution
            s_dirty = true;
        }
    }
    if (s_editLen && enter)
    {
        int v = 0;
        for (int i = 0; i < s_editLen; i++)
            v = v * 10 + (s_edit[i] - '0');
        s_editLen = 0;
        SetScalePercent(v);
        return;
    }
    switch (s_sel)
    {
    case kItemScale:
        if (dir)
        {
            int step = shift ? 10 : 1;
            int cur = CurrentPercent();
            SetScalePercent(cur + dir * step);
        }
        break;
    case kItemPreset:
        if (dir)
        {
            int idx = 0;
            for (int i = 0; i < kNumPresets; i++)
                if (kPresets[i] == g_cfg.preset)
                    idx = i;
            idx = (idx + dir + kNumPresets) % kNumPresets;
            g_cfg.preset = kPresets[idx];
            s_dirty = true;
        }
        break;
    case kItemDlss:
        if (dir || enter)
            ModSetDlssOn(!ModGetDlssOn());
        break;
    case kItemSave:
        if (enter)
            SaveIni();
        break;
    }
}

// ---------------------------------------------------------------- text grid
enum
{
    kCols = 64,
    kRows = 14
};
static uint32_t s_grid[kCols * kRows];
static int s_rowsUsed, s_colsUsed;
enum
{
    cWhite = 0,
    cGray = 1,
    cGreen = 2,
    cYellow = 3,
    cCyan = 4,
    cRed = 5
};
static void Put(int row, int col, const char* s, int color)
{
    for (; *s && col < kCols; s++, col++)
        s_grid[row * kCols + col] = (uint8_t)*s | (color << 8);
    if (col > s_colsUsed)
        s_colsUsed = col;
    if (row + 1 > s_rowsUsed)
        s_rowsUsed = row + 1;
}

static void BuildMenu()
{
    memset(s_grid, 0, sizeof(s_grid));
    s_rowsUsed = s_colsUsed = 0;
    char b[128];
    UINT lw = 0, lh = 0, fw = 0, fh = 0;
    bool active = false, seen = false;
    ModGetStatus(&lw, &lh, &fw, &fh, &active, &seen);

    Put(0, 1, "GRB-DLSS " MOD_VERSION, cCyan);
    if (active)
        _snprintf(b, sizeof(b), "DLSS active  %ux%u -> %ux%u", lw, lh, fw, fh);
    else if (seen)
        _snprintf(b, sizeof(b), "Game TAA  %ux%u -> %ux%u", lw, lh, fw, fh);
    else
        _snprintf(b, sizeof(b), "waiting for the game's TAA pass");
    Put(0, 20, b, active ? cGreen : cYellow);

    float s = ModGetRenderScale();
    int pct = CurrentPercent();
    int row = 2;
    // render resolution
    {
        bool sel = s_sel == kItemScale;
        Put(row, 1, sel ? ">" : " ", cYellow);
        Put(row, 3, "Render resolution", cWhite);
        if (s_editLen)
            _snprintf(b, sizeof(b), "< %s_ %% >", s_edit);
        else if (s > 0.0f)
            _snprintf(b, sizeof(b), "< %d %% >", pct);
        else
            _snprintf(b, sizeof(b), "< game (%d %%) >", pct);
        Put(row, 24, b, sel ? cYellow : cWhite);
        row++;
        if (fw && fh)
            _snprintf(b, sizeof(b), "%ux%u  (%s)", (UINT)(fw * (pct / 100.0f)), (UINT)(fh * (pct / 100.0f)),
                      QualityName(pct / 100.0f));
        else
            _snprintf(b, sizeof(b), "(%s)", QualityName(pct / 100.0f));
        Put(row, 24, b, cGray);
        row++;
    }
    {
        bool sel = s_sel == kItemPreset;
        Put(row, 1, sel ? ">" : " ", cYellow);
        Put(row, 3, "DLSS preset", cWhite);
        _snprintf(b, sizeof(b), "< %s >", PresetName(g_cfg.preset));
        Put(row, 24, b, sel ? cYellow : cWhite);
        row++;
    }
    {
        bool sel = s_sel == kItemDlss;
        Put(row, 1, sel ? ">" : " ", cYellow);
        Put(row, 3, "DLSS", cWhite);
        Put(row, 24, ModGetDlssOn() ? "< on >" : "< off (game TAA) >", sel ? cYellow : cWhite);
        row++;
    }
    {
        bool sel = s_sel == kItemSave;
        Put(row, 1, sel ? ">" : " ", cYellow);
        Put(row, 3, s_dirty ? "Save to GRB_DLSS.ini (Enter)  *unsaved*" : "Save to GRB_DLSS.ini (Enter)", sel ? cYellow : cWhite);
        row++;
    }
    row++;
    Put(row++, 1, "Up/Down select   Left/Right change (Shift: 10%)", cGray);
    Put(row++, 1, "Type digits + Enter for an exact %   Del: game default", cGray);
    Put(row++, 1, "The game's Resolution Scale sets the output size", cGray);
    Put(row++, 1, "Home or Esc closes (changes are saved)", cGray);
}

static void BuildToast()
{
    memset(s_grid, 0, sizeof(s_grid));
    s_rowsUsed = s_colsUsed = 0;
    Put(0, 1, s_toast, cWhite);
    s_colsUsed += 1;
}

// ---------------------------------------------------------------- rendering
static const char kOverlayHlsl[] = R"(
cbuffer C : register(b0) { float2 gOrigin; float2 gCell; uint gCols; uint gRows; float gHi; float gPad; };
Buffer<uint> gText : register(t0);
Texture2D<float> gFont : register(t1);
float4 vsmain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 Pal(uint c)
{
    if (c == 1) return float3(0.62, 0.64, 0.68);
    if (c == 2) return float3(0.46, 0.88, 0.46);
    if (c == 3) return float3(1.00, 0.84, 0.36);
    if (c == 4) return float3(0.45, 0.80, 1.00);
    if (c == 5) return float3(1.00, 0.42, 0.40);
    return float3(0.93, 0.94, 0.96);
}
float4 psmain(float4 pos : SV_Position) : SV_Target
{
    float2 p = pos.xy - gOrigin;
    int2 cell = int2(floor(p / gCell));
    float4 bg = float4(0.035, 0.04, 0.055, 0.88);
    if (cell.y == (int)gHi) bg = float4(0.09, 0.20, 0.36, 0.94);
    if (cell.x < 0 || cell.y < 0 || cell.x >= (int)gCols || cell.y >= (int)gRows) return bg;
    uint v = gText[cell.y * gCols + cell.x];
    uint ch = v & 0xFF;
    float a = 0;
    if (ch > 32 && ch < 127)
    {
        float2 f = (p - cell * gCell) / gCell;
        int2 g = int2((ch - 32) * 12 + min(11, (int)(f.x * 12)), min(23, (int)(f.y * 24)));
        a = gFont.Load(int3(g, 0));
    }
    float3 c = lerp(bg.rgb, Pal((v >> 8) & 15), a);
    return float4(c, max(bg.a, a));
}
)";

static ID3D11Device* s_odev;
static ID3D11VertexShader* s_ovs;
static ID3D11PixelShader* s_ops;
static ID3D11Buffer *s_ocb, *s_otext;
static ID3D11ShaderResourceView *s_otextSRV, *s_ofontSRV;
static ID3D11BlendState* s_oblend;
static ID3D11RasterizerState* s_orast;
static ID3D11DepthStencilState* s_odepth;
static int s_ostate; // 0 not init, 1 ok, -1 failed

typedef HRESULT(WINAPI* PFN_D3DCompile2)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                        ID3DBlob**);

static bool InitRender(ID3D11Device* dev)
{
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_modDir);
    lstrcatW(p, L"d3dcompiler_47.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m)
        m = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile2 compile = m ? (PFN_D3DCompile2)GetProcAddress(m, "D3DCompile") : nullptr;
    if (!compile)
        return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "GRB_DLSS_Menu", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vsb, &err);
    if (FAILED(hr))
    {
        LOGE("Menu VS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    hr = compile(kOverlayHlsl, sizeof(kOverlayHlsl) - 1, "GRB_DLSS_Menu", nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &psb, &err);
    if (FAILED(hr))
    {
        LOGE("Menu PS compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        SafeRelease(vsb);
        return false;
    }
    SafeRelease(err);
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s_ovs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s_ops);
    SafeRelease(vsb);
    SafeRelease(psb);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    dev->CreateBuffer(&bd, nullptr, &s_ocb);
    bd.ByteWidth = sizeof(s_grid);
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateBuffer(&bd, nullptr, &s_otext);
    if (s_otext)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = kCols * kRows;
        dev->CreateShaderResourceView(s_otext, &sd, &s_otextSRV);
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kFontW;
    td.Height = kFontCH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {kFontAtlas, (UINT)kFontW, 0};
    ID3D11Texture2D* ft = nullptr;
    dev->CreateTexture2D(&td, &init, &ft);
    if (ft)
    {
        dev->CreateShaderResourceView(ft, nullptr, &s_ofontSRV);
        ft->Release();
    }
    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &s_oblend);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &s_orast);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dev->CreateDepthStencilState(&dd, &s_odepth);
    bool ok = s_ovs && s_ops && s_ocb && s_otextSRV && s_ofontSRV && s_oblend && s_orast && s_odepth;
    LOGI("Menu renderer %s", ok ? "ready" : "FAILED");
    return ok;
}

struct SavedState
{
    ID3D11RenderTargetView* rtv[8];
    ID3D11DepthStencilView* dsv;
    D3D11_VIEWPORT vp[16];
    UINT nvp;
    D3D11_RECT sc[16];
    UINT nsc;
    ID3D11RasterizerState* rs;
    ID3D11BlendState* bs;
    float bf[4];
    UINT sm;
    ID3D11DepthStencilState* ds;
    UINT sref;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11InputLayout* il;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs;
    ID3D11HullShader* hs;
    ID3D11DomainShader* dsh;
    ID3D11ShaderResourceView* srv[2];
    ID3D11Buffer* cb;
};

static void Save(ID3D11DeviceContext* c, SavedState& s)
{
    memset(&s, 0, sizeof(s));
    c->OMGetRenderTargets(8, s.rtv, &s.dsv);
    s.nvp = 16;
    c->RSGetViewports(&s.nvp, s.vp);
    s.nsc = 16;
    c->RSGetScissorRects(&s.nsc, s.sc);
    c->RSGetState(&s.rs);
    c->OMGetBlendState(&s.bs, s.bf, &s.sm);
    c->OMGetDepthStencilState(&s.ds, &s.sref);
    c->IAGetPrimitiveTopology(&s.topo);
    c->IAGetInputLayout(&s.il);
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr);
    c->HSGetShader(&s.hs, nullptr, nullptr);
    c->DSGetShader(&s.dsh, nullptr, nullptr);
    c->PSGetShaderResources(0, 2, s.srv);
    c->PSGetConstantBuffers(0, 1, &s.cb);
}

static void Restore(ID3D11DeviceContext* c, SavedState& s)
{
    c->OMSetRenderTargets(8, s.rtv, s.dsv);
    c->RSSetViewports(s.nvp, s.vp);
    c->RSSetScissorRects(s.nsc, s.sc);
    c->RSSetState(s.rs);
    c->OMSetBlendState(s.bs, s.bf, s.sm);
    c->OMSetDepthStencilState(s.ds, s.sref);
    c->IASetPrimitiveTopology(s.topo);
    c->IASetInputLayout(s.il);
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0);
    c->HSSetShader(s.hs, nullptr, 0);
    c->DSSetShader(s.dsh, nullptr, 0);
    c->PSSetShaderResources(0, 2, s.srv);
    c->PSSetConstantBuffers(0, 1, &s.cb);
    for (auto*& r : s.rtv)
        SafeRelease(r);
    SafeRelease(s.dsv);
    SafeRelease(s.rs);
    SafeRelease(s.bs);
    SafeRelease(s.ds);
    SafeRelease(s.il);
    SafeRelease(s.vs);
    SafeRelease(s.ps);
    SafeRelease(s.gs);
    SafeRelease(s.hs);
    SafeRelease(s.dsh);
    for (auto*& r : s.srv)
        SafeRelease(r);
    SafeRelease(s.cb);
}

static void Draw(IDXGISwapChain* sc, bool toast)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb)
        return;
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    if (!dev)
    {
        bb->Release();
        return;
    }
    if (s_ostate == 0)
    {
        s_odev = dev;
        s_ostate = InitRender(dev) ? 1 : -1;
    }
    if (s_ostate != 1 || dev != s_odev)
    {
        dev->Release();
        bb->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = td.Format;
    if (td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    else if (td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS)
        rd.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    else if (td.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS)
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, &rd, &rtv);
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (rtv && ctx)
    {
        if (toast)
            BuildToast();
        else
            BuildMenu();
        int scale = td.Height >= 2000 ? 2 : 1;
        float cw = (float)(kFontCW * scale), ch = (float)(kFontCH * scale);
        float pad = 12.0f * scale;
        int cols = s_colsUsed + 1, rows = s_rowsUsed;
        float w = cols * cw + 2 * pad, h = rows * ch + 2 * pad;
        float x0 = 40.0f * scale, y0 = 40.0f * scale;
        struct
        {
            float ox, oy, cw, ch;
            uint32_t cols, rows;
            float hi, pad;
            float r[4];
        } cbd = {x0 + pad, y0 + pad, cw, ch, (uint32_t)kCols, (uint32_t)rows, -100.0f, 0, {0, 0, 0, 0}};
        if (!toast)
        {
            // highlight the selected row (render-resolution item spans two rows: highlight the first)
            int hiRow = s_sel == kItemScale ? 2 : s_sel == kItemPreset ? 4 : s_sel == kItemDlss ? 5 : 6;
            cbd.hi = (float)hiRow;
        }
        ModSetOverlayThread(true);
        SavedState st;
        Save(ctx, st);
        ctx->UpdateSubresource(s_ocb, 0, nullptr, &cbd, 0, 0);
        ctx->UpdateSubresource(s_otext, 0, nullptr, s_grid, 0, 0);
        D3D11_VIEWPORT vp = {x0, y0, w, h, 0.0f, 1.0f};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(s_orast);
        float bf[4] = {0, 0, 0, 0};
        ctx->OMSetBlendState(s_oblend, bf, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(s_odepth, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(nullptr);
        ctx->VSSetShader(s_ovs, nullptr, 0);
        ctx->PSSetShader(s_ops, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = {s_otextSRV, s_ofontSRV};
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetConstantBuffers(0, 1, &s_ocb);
        ctx->Draw(3, 0);
        Restore(ctx, st);
        ModSetOverlayThread(false);
    }
    SafeRelease(rtv);
    SafeRelease(ctx);
    dev->Release();
    bb->Release();
}

// ---------------------------------------------------------------- swap chain hooks
static volatile LONG s_presentLogged;
volatile LONG64 g_presentCount;
void ModOnPresentEvent();
static void OnPresent(IDXGISwapChain* sc)
{
    InterlockedIncrement64(&g_presentCount);
    ModOnPresentEvent();
    if (!InterlockedExchange(&s_presentLogged, 1))
        LOGI("Menu: first Present on swap chain %p", sc);
    static IDXGISwapChain* lastSc;
    if (sc != lastSc)
    {
        // track the window of the swap chain the game actually presents with
        DXGI_SWAP_CHAIN_DESC d;
        if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow)
        {
            if (d.OutputWindow != s_hwnd)
                LOGI("Menu: game presents to window %p", d.OutputWindow);
            s_hwnd = d.OutputWindow;
            SubclassWindow(d.OutputWindow);
        }
        lastSc = sc;
    }
    HandleInput(g_cfg.menuKey);
    bool toast = !s_open && s_toast[0] && NowQpc() < s_toastUntil;
    if (s_open || toast)
        Draw(sc, toast);
}

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static volatile LONG s_inPresent;
static HRESULT STDMETHODCALLTYPE Hk_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    PFN_Present o = (PFN_Present)OrigFor(sc, 8);
    // only the outermost Present of a frame draws (wrappers call into the real swap chain)
    if (InterlockedIncrement(&s_inPresent) == 1 && !(flags & DXGI_PRESENT_TEST))
        OnPresent(sc);
    HRESULT hr = o(sc, sync, flags);
    InterlockedDecrement(&s_inPresent);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    PFN_Present1 o = (PFN_Present1)OrigFor(sc, 22);
    if (InterlockedIncrement(&s_inPresent) == 1 && !(flags & DXGI_PRESENT_TEST))
        OnPresent(sc);
    HRESULT hr = o(sc, sync, flags, pp);
    InterlockedDecrement(&s_inPresent);
    return hr;
}

static void OnSwapChain(IUnknown* obj)
{
    IDXGISwapChain* sc = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc)) || !sc)
        return;
    // only D3D11 swap chains (the game's); skip D3D12 ones created by frame generation
    ID3D11Device* d = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d)) || !d)
    {
        LOGI("Menu: swap chain %p is not a D3D11 one - not hooked", sc);
        sc->Release();
        return;
    }
    d->Release();
    HookSlot(sc, 8, (void*)Hk_Present);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1)
    {
        HookSlot(sc1, 22, (void*)Hk_Present1);
        sc1->Release();
    }
    DXGI_SWAP_CHAIN_DESC desc;
    if (SUCCEEDED(sc->GetDesc(&desc)))
    {
        LOGI("Menu: hooked swap chain %p (vtable %p) %ux%u window %p", sc, *(void**)sc, desc.BufferDesc.Width,
             desc.BufferDesc.Height, desc.OutputWindow);
        SubclassWindow(desc.OutputWindow);
    }
    sc->Release();
}

static volatile LONG s_scDepth;
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSC)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSCHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
void LogCaller(const char* what, void* ret, void* obj);
static volatile LONG s_scLogs;
static HRESULT STDMETHODCALLTYPE Hk_CreateSC(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out)
{
    PFN_CreateSC o = (PFN_CreateSC)OrigFor(f, 10);
    LONG nest = EnterDepth();
    bool log = InterlockedIncrement(&s_scLogs) <= 12;
    if (log)
        LogCaller("CreateSwapChain begin", __builtin_return_address(0), f);
    if (nest > 1)
    {
        PFN_CreateSC g = (PFN_CreateSC)GenuineFor(f, 10);
        if (log || nest == 2)
            LOGI("CreateSwapChain re-entered (depth %ld): calling the system implementation %p directly", nest, g);
        if (g)
            o = g;
        else if (nest > 3)
            o = nullptr; // no known way out of the loop: fail the call instead of overflowing the stack
    }
    if (!o)
    {
        LeaveDepth();
        return DXGI_ERROR_INVALID_CALL;
    }
    LONG depth = InterlockedIncrement(&s_scDepth);
    HRESULT hr = o(f, dev, d, out);
    InterlockedDecrement(&s_scDepth);
    LeaveDepth();
    if (log)
        LOGI("CreateSwapChain end hr=0x%08lX sc=%p", hr, out ? *out : nullptr);
    if (depth == 1 && SUCCEEDED(hr) && out && *out)
        OnSwapChain(*out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateSCHwnd(IDXGIFactory2* f, IUnknown* dev, HWND h, const DXGI_SWAP_CHAIN_DESC1* d,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o1, IDXGISwapChain1** out)
{
    PFN_CreateSCHwnd o = (PFN_CreateSCHwnd)OrigFor(f, 15);
    LONG nest = EnterDepth();
    bool log = InterlockedIncrement(&s_scLogs) <= 12;
    if (log)
        LogCaller("CreateSwapChainForHwnd begin", __builtin_return_address(0), f);
    if (nest > 1)
    {
        PFN_CreateSCHwnd g = (PFN_CreateSCHwnd)GenuineFor(f, 15);
        if (log || nest == 2)
            LOGI("CreateSwapChainForHwnd re-entered (depth %ld): calling the system implementation %p directly", nest, g);
        if (g)
            o = g;
        else if (nest > 3)
            o = nullptr;
    }
    if (!o)
    {
        LeaveDepth();
        return DXGI_ERROR_INVALID_CALL;
    }
    LONG depth = InterlockedIncrement(&s_scDepth);
    HRESULT hr = o(f, dev, h, d, fs, o1, out);
    InterlockedDecrement(&s_scDepth);
    LeaveDepth();
    if (log)
        LOGI("CreateSwapChainForHwnd end hr=0x%08lX sc=%p", hr, out ? *out : nullptr);
    if (depth == 1 && SUCCEEDED(hr) && out && *out)
        OnSwapChain(*out);
    return hr;
}

static void HookFactory(IUnknown* obj)
{
    if (!obj)
        return;
    IDXGIFactory* f = nullptr;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(IDXGIFactory), (void**)&f)) && f)
    {
        HookSlot(f, 10, (void*)Hk_CreateSC);
        IDXGIFactory2* f2 = nullptr;
        if (SUCCEEDED(f->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2)) && f2)
        {
            HookSlot(f2, 15, (void*)Hk_CreateSCHwnd);
            f2->Release();
        }
        f->Release();
    }
}

void OverlayHookDeviceFactory(ID3D11Device* dev)
{
    IDXGIDevice* dd = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) || !dd)
        return;
    IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dd->GetAdapter(&ad)) && ad)
    {
        IDXGIFactory* f = nullptr;
        if (SUCCEEDED(ad->GetParent(__uuidof(IDXGIFactory), (void**)&f)) && f)
        {
            HookFactory(f);
            f->Release();
        }
        ad->Release();
    }
    dd->Release();
}

typedef HRESULT(WINAPI* PFN_CreateFactory)(REFIID, void**);
typedef HRESULT(WINAPI* PFN_CreateFactory2)(UINT, REFIID, void**);
static PFN_CreateFactory o_CreateFactory, o_CreateFactory1;
static PFN_CreateFactory2 o_CreateFactory2;
static HRESULT WINAPI Hk_CreateFactory(REFIID r, void** out)
{
    HRESULT hr = o_CreateFactory(r, out);
    if (SUCCEEDED(hr) && out)
        HookFactory((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI Hk_CreateFactory1(REFIID r, void** out)
{
    HRESULT hr = o_CreateFactory1(r, out);
    if (SUCCEEDED(hr) && out)
        HookFactory((IUnknown*)*out);
    return hr;
}
static HRESULT WINAPI Hk_CreateFactory2(UINT fl, REFIID r, void** out)
{
    HRESULT hr = o_CreateFactory2(fl, r, out);
    if (SUCCEEDED(hr) && out)
        HookFactory((IUnknown*)*out);
    return hr;
}

void OverlayInit()
{
    if (!g_cfg.menuKey)
    {
        LOGI("Menu disabled (MenuKey=0)");
        return;
    }
    s_depthTls = TlsAlloc();
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi)
        return;
    // when OptiScaler detours the dxgi exports, sit in front of it so we see what the game gets
    ChainExport(dxgi, "CreateDXGIFactory", (void*)Hk_CreateFactory, (void**)&o_CreateFactory);
    ChainExport(dxgi, "CreateDXGIFactory1", (void*)Hk_CreateFactory1, (void**)&o_CreateFactory1);
    ChainExport(dxgi, "CreateDXGIFactory2", (void*)Hk_CreateFactory2, (void**)&o_CreateFactory2);
    // and hook the factory class we get ourselves (covers the plain, un-detoured case)
    PFN_CreateFactory cf1 = (PFN_CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IUnknown* f = nullptr;
    if (cf1 && SUCCEEDED(cf1(__uuidof(IDXGIFactory1), (void**)&f)) && f)
    {
        HookFactory(f);
        f->Release();
    }
    LOGI("Menu: swap chain creation hooks installed (menu key 0x%X)", g_cfg.menuKey);
}
