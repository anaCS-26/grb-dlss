// GRB DLSS - DLSS Super Resolution injection for Ghost Recon Breakpoint (DX11)
// Freestanding build (no static CRT): only Win32 + msvcrt.dll imports.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stddef.h>

extern "C" {
int _vsnprintf(char* buf, size_t n, const char* fmt, va_list ap);
int _snprintf(char* buf, size_t n, const char* fmt, ...);
void* memcpy(void*, const void*, size_t);
void* memset(void*, int, size_t);
int memcmp(const void*, const void*, size_t);
size_t strlen(const char*);
int strcmp(const char*, const char*);

size_t wcslen(const wchar_t*);
wchar_t* wcsrchr(const wchar_t*, wchar_t);
double atan(double);
double tan(double);
double floor(double);
double fabs(double);
double sqrt(double);
}

#define MOD_NAME "GRB-DLSS"
#define MOD_VERSION "1.1.0"

// ---------------------------------------------------------------- logging
enum { LOG_ERR = 0, LOG_INFO = 1, LOG_DBG = 2, LOG_TRACE = 3 };
void LogInit(const wchar_t* path);
void Log(int level, const char* fmt, ...);
#define LOGE(...) Log(LOG_ERR, __VA_ARGS__)
#define LOGI(...) Log(LOG_INFO, __VA_ARGS__)
#define LOGD(...) Log(LOG_DBG, __VA_ARGS__)
#define LOGT(...) Log(LOG_TRACE, __VA_ARGS__)

// ---------------------------------------------------------------- config
struct Config
{
    int enabled;          // master switch (DLSS replaces TAA)
    int toggleKey;        // VK code to toggle DLSS <-> game TAA at runtime (0 = none)
    int preset;           // 0 = default, 1..15 = A..O
    float jitterSignX;    // multiplier applied to the game's SubPixelJitter
    float jitterSignY;
    int depthInverted;    // -1 auto, 0 no, 1 yes
    int useBiasMask;      // feed the game's ResponsiveAA mask as DLSS bias-current-colour mask
    int autoExposure;     // only used in HDR mode
    int logLevel;
    int ngxLogging;       // forward NGX's own log lines into our log
    int qualityOverride;  // -1 auto (from the game's resolution scale), else NVSDK_NGX_PerfQuality_Value
    float sharpness;      // legacy DLSS sharpening (ignored by DLSS 2.5.1+)
    int debugView;        // 0 off, 1 motion vectors, 2 depth
    int hookDelayMs;
    int devCommands;
    int shadowDiag;
    int presetKey;
    int maskFeatureLevel;
    float renderScale;    // 0 = game decides (Temporal Injection = 0.667), else fraction of the output resolution
    int renderScaleUpKey;
    int renderScaleDownKey;
    float renderScaleStep;
    int menuKey;
    int dilatedMV;
    int fgCameraData;     // 1: feed the game's real camera matrices to Streamline frame generation        // 1: full-resolution depth-dilated motion vectors for DLSS / frame generation
};
extern Config g_cfg;
extern wchar_t g_modDir[MAX_PATH];
void LoadConfig();

// ---------------------------------------------------------------- misc helpers
template <class T> static inline void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}
static inline uint64_t NowQpc()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (uint64_t)li.QuadPart;
}
double QpcToMs(uint64_t delta);

// mod entry points
void ModBootstrap();
