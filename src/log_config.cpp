#include "common.h"
#include <stdarg.h>

Config g_cfg;
wchar_t g_modDir[MAX_PATH];

static HANDLE s_logFile = INVALID_HANDLE_VALUE;
static SRWLOCK s_logLock = SRWLOCK_INIT;
static uint64_t s_qpcStart, s_qpcFreq;

double QpcToMs(uint64_t delta)
{
    return (double)delta * 1000.0 / (double)s_qpcFreq;
}

void LogInit(const wchar_t* path)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    s_qpcFreq = f.QuadPart;
    s_qpcStart = c.QuadPart;
    s_logFile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(int level, const char* fmt, ...)
{
    if (level > g_cfg.logLevel || s_logFile == INVALID_HANDLE_VALUE)
        return;
    static char buf[4096];
    AcquireSRWLockExclusive(&s_logLock);
    double t = QpcToMs(NowQpc() - s_qpcStart) / 1000.0;
    static const char* lv[] = {"ERROR", "INFO ", "DEBUG", "TRACE"};
    int n = _snprintf(buf, sizeof(buf) - 2, "[%9.3f] [%5lu] %s | ", t, GetCurrentThreadId(), lv[level & 3]);
    if (n < 0)
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(buf + n, sizeof(buf) - 2 - n, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = (int)(sizeof(buf) - 2 - n);
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    DWORD w;
    WriteFile(s_logFile, buf, n, &w, nullptr);
    ReleaseSRWLockExclusive(&s_logLock);
}

static int ReadInt(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, int def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
    if (!buf[0])
        return def;
    // accept hex (0x..), decimal, negative
    int sign = 1, i = 0;
    if (buf[0] == L'-')
    {
        sign = -1;
        i = 1;
    }
    int v = 0;
    if (buf[i] == L'0' && (buf[i + 1] == L'x' || buf[i + 1] == L'X'))
    {
        i += 2;
        for (; buf[i]; i++)
        {
            wchar_t ch = buf[i];
            int d = (ch >= L'0' && ch <= L'9') ? ch - L'0' : (ch >= L'a' && ch <= L'f') ? ch - L'a' + 10 : (ch >= L'A' && ch <= L'F') ? ch - L'A' + 10 : -1;
            if (d < 0)
                break;
            v = v * 16 + d;
        }
    }
    else
    {
        if (!(buf[i] >= L'0' && buf[i] <= L'9'))
            return def;
        for (; buf[i] >= L'0' && buf[i] <= L'9'; i++)
            v = v * 10 + (buf[i] - L'0');
    }
    return v * sign;
}

static float ReadFloat(const wchar_t* ini, const wchar_t* sec, const wchar_t* key, float def)
{
    wchar_t buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
    if (!buf[0])
        return def;
    double sign = 1, v = 0, frac = 0.1;
    int i = 0;
    if (buf[0] == L'-')
    {
        sign = -1;
        i = 1;
    }
    else if (buf[0] == L'+')
        i = 1;
    bool any = false;
    for (; buf[i] >= L'0' && buf[i] <= L'9'; i++, any = true)
        v = v * 10 + (buf[i] - L'0');
    if (buf[i] == L'.')
        for (i++; buf[i] >= L'0' && buf[i] <= L'9'; i++, any = true, frac *= 0.1)
            v += (buf[i] - L'0') * frac;
    return any ? (float)(v * sign) : def;
}

static int ReadPreset(const wchar_t* ini)
{
    wchar_t buf[32];
    GetPrivateProfileStringW(L"DLSS", L"Preset", L"", buf, 32, ini);
    if (!buf[0])
        return 0;
    wchar_t c = buf[0];
    if (c >= L'a' && c <= L'z')
        c = c - L'a' + L'A';
    if (c >= L'A' && c <= L'O' && buf[1] == 0)
        return 1 + (c - L'A');
    int v = 0;
    for (int i = 0; buf[i] >= L'0' && buf[i] <= L'9'; i++)
        v = v * 10 + (buf[i] - L'0');
    return v;
}

static int ReadQuality(const wchar_t* ini)
{
    wchar_t buf[32];
    GetPrivateProfileStringW(L"DLSS", L"QualityMode", L"auto", buf, 32, ini);
    struct
    {
        const wchar_t* n;
        int v;
    } map[] = {{L"performance", 0}, {L"balanced", 1}, {L"quality", 2}, {L"ultraperformance", 3}, {L"ultraquality", 4}, {L"dlaa", 5}};
    for (auto& m : map)
        if (lstrcmpiW(buf, m.n) == 0)
            return m.v;
    return -1;
}

static float ReadScale(const wchar_t* ini)
{
    wchar_t buf[32];
    GetPrivateProfileStringW(L"DLSS", L"RenderScale", L"0", buf, 32, ini);
    if (lstrcmpiW(buf, L"off") == 0 || lstrcmpiW(buf, L"game") == 0 || lstrcmpiW(buf, L"auto") == 0)
        return 0.0f;
    float v = ReadFloat(ini, L"DLSS", L"RenderScale", 0.0f);
    if (v > 1.5f) // given in percent
        v /= 100.0f;
    if (v <= 0.0f)
        return 0.0f;
    if (v < 0.25f)
        v = 0.25f;
    if (v > 1.0f)
        v = 1.0f;
    return v;
}

void LoadConfig()
{
    wchar_t ini[MAX_PATH];
    lstrcpyW(ini, g_modDir);
    lstrcatW(ini, L"GRB_DLSS.ini");

    g_cfg.enabled = ReadInt(ini, L"DLSS", L"Enabled", 1);
    g_cfg.toggleKey = ReadInt(ini, L"DLSS", L"ToggleKey", 0x23 /* End */);
    g_cfg.preset = ReadPreset(ini);
    g_cfg.presetKey = ReadInt(ini, L"DLSS", L"PresetCycleKey", 0);
    g_cfg.menuKey = ReadInt(ini, L"DLSS", L"MenuKey", 0x24 /* Home */);
    if (g_cfg.presetKey == g_cfg.menuKey)
        g_cfg.presetKey = 0;
    g_cfg.qualityOverride = ReadQuality(ini);
    g_cfg.renderScale = ReadScale(ini);
    g_cfg.renderScaleUpKey = ReadInt(ini, L"DLSS", L"RenderScaleUpKey", 0x21 /* Page Up */);
    g_cfg.renderScaleDownKey = ReadInt(ini, L"DLSS", L"RenderScaleDownKey", 0x22 /* Page Down */);
    g_cfg.renderScaleStep = ReadFloat(ini, L"DLSS", L"RenderScaleStep", 5.0f);
    if (g_cfg.renderScaleStep > 0.5f)
        g_cfg.renderScaleStep /= 100.0f;
    g_cfg.sharpness = ReadFloat(ini, L"DLSS", L"Sharpness", 0.0f);
    g_cfg.useBiasMask = ReadInt(ini, L"DLSS", L"UseResponsiveMask", 0);
    g_cfg.dilatedMV = ReadInt(ini, L"DLSS", L"DilatedMotionVectors", 1);
    g_cfg.autoExposure = ReadInt(ini, L"DLSS", L"AutoExposureHDR", 1);
    g_cfg.jitterSignX = ReadFloat(ini, L"Advanced", L"JitterScaleX", -1.0f);
    g_cfg.jitterSignY = ReadFloat(ini, L"Advanced", L"JitterScaleY", -1.0f);
    g_cfg.depthInverted = ReadInt(ini, L"Advanced", L"DepthInverted", -1);
    g_cfg.debugView = ReadInt(ini, L"Advanced", L"DebugView", 0);
    g_cfg.hookDelayMs = ReadInt(ini, L"Advanced", L"HookDelayMs", 0);
    g_cfg.logLevel = ReadInt(ini, L"Log", L"LogLevel", 1);
    g_cfg.ngxLogging = ReadInt(ini, L"Log", L"NGXLogging", 0);
    g_cfg.maskFeatureLevel = ReadInt(ini, L"Compat", L"ReportRequestedFeatureLevel", 1);
    g_cfg.fgCameraData = ReadInt(ini, L"Compat", L"FGCameraData", 1);
    g_cfg.devCommands = ReadInt(ini, L"Log", L"DevCommands", 0);
    g_cfg.shadowDiag = ReadInt(ini, L"Log", L"ShadowDiag", 0);
}
