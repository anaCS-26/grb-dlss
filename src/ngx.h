// Minimal NVIDIA NGX (DLSS) D3D11 interface, loaded dynamically from the driver's NGX core
// (or from OptiScaler when it is installed: OptiScaler intercepts the LoadLibrary of _nvngx.dll).
#pragma once
#include "common.h"

typedef unsigned int NVSDK_NGX_Result;
#define NVSDK_NGX_Result_Success 0x1u
#define NGX_OK(r) ((((unsigned)(r)) & 0xFFF00000u) != 0xBAD00000u)

#define NVSDK_NGX_Version_API 0x0000015
#define NVSDK_NGX_Feature_SuperSampling 1
#define NVSDK_NGX_ENGINE_TYPE_CUSTOM 0

enum
{
    NGX_PQ_MaxPerf = 0,
    NGX_PQ_Balanced = 1,
    NGX_PQ_MaxQuality = 2,
    NGX_PQ_UltraPerformance = 3,
    NGX_PQ_UltraQuality = 4,
    NGX_PQ_DLAA = 5,
};

enum
{
    NGX_DLSS_IsHDR = 1 << 0,
    NGX_DLSS_MVLowRes = 1 << 1,
    NGX_DLSS_MVJittered = 1 << 2,
    NGX_DLSS_DepthInverted = 1 << 3,
    NGX_DLSS_DoSharpening = 1 << 5,
    NGX_DLSS_AutoExposure = 1 << 6,
};

struct NVSDK_NGX_Handle
{
    unsigned int Id;
};

// NVSDK_NGX_Parameter is an MSVC C++ interface with overloaded virtuals. MSVC lays out
// overloads in *reverse* declaration order, so the table below is in the real slot order
// (verified with clang -fdump-vtable-layouts --target=x86_64-pc-windows-msvc).
struct NgxParamVtbl
{
    void (*SetVoidPointer)(void* self, const char* n, void* v);
    void (*SetD3d12Resource)(void* self, const char* n, void* v);
    void (*SetD3d11Resource)(void* self, const char* n, ID3D11Resource* v);
    void (*SetI)(void* self, const char* n, int v);
    void (*SetUI)(void* self, const char* n, unsigned int v);
    void (*SetD)(void* self, const char* n, double v);
    void (*SetF)(void* self, const char* n, float v);
    void (*SetULL)(void* self, const char* n, unsigned long long v);
    NVSDK_NGX_Result (*GetVoidPointer)(void* self, const char* n, void** v);
    NVSDK_NGX_Result (*GetD3d12Resource)(void* self, const char* n, void** v);
    NVSDK_NGX_Result (*GetD3d11Resource)(void* self, const char* n, ID3D11Resource** v);
    NVSDK_NGX_Result (*GetI)(void* self, const char* n, int* v);
    NVSDK_NGX_Result (*GetUI)(void* self, const char* n, unsigned int* v);
    NVSDK_NGX_Result (*GetD)(void* self, const char* n, double* v);
    NVSDK_NGX_Result (*GetF)(void* self, const char* n, float* v);
    NVSDK_NGX_Result (*GetULL)(void* self, const char* n, unsigned long long* v);
    void (*Reset)(void* self);
};
struct NVSDK_NGX_Parameter
{
    NgxParamVtbl* vt;
    void SetUI(const char* n, unsigned v) { vt->SetUI(this, n, v); }
    void SetI(const char* n, int v) { vt->SetI(this, n, v); }
    void SetF(const char* n, float v) { vt->SetF(this, n, v); }
    void SetULL(const char* n, unsigned long long v) { vt->SetULL(this, n, v); }
    void SetRes(const char* n, ID3D11Resource* r) { vt->SetD3d11Resource(this, n, r); }
    void SetPtr(const char* n, void* p) { vt->SetVoidPointer(this, n, p); }
    NVSDK_NGX_Result GetI(const char* n, int* v) { return vt->GetI(this, n, v); }
    NVSDK_NGX_Result GetUI(const char* n, unsigned* v) { return vt->GetUI(this, n, v); }
};

struct NVSDK_NGX_PathListInfo
{
    const wchar_t* const* Path;
    unsigned int Length;
};
typedef void (*NVSDK_NGX_AppLogCallback)(const char* message, int loggingLevel, int sourceComponent);
struct NVSDK_NGX_LoggingInfo
{
    NVSDK_NGX_AppLogCallback LoggingCallback;
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
};
struct NVSDK_NGX_FeatureCommonInfo
{
    NVSDK_NGX_PathListInfo PathListInfo;
    void* InternalData;
    NVSDK_NGX_LoggingInfo LoggingInfo;
};

// Exports of _nvngx.dll (driver NGX core) / OptiScaler. Note the *core* argument order:
// version comes before feature info for _Ext / _ProjectID.
typedef NVSDK_NGX_Result (*PFN_D3D11_Init_Ext)(unsigned long long appId, const wchar_t* dataPath, ID3D11Device* dev,
                                               unsigned int sdkVersion, const NVSDK_NGX_FeatureCommonInfo* info);
typedef NVSDK_NGX_Result (*PFN_D3D11_Init_ProjectID)(const char* projectId, int engineType, const char* engineVersion,
                                                     const wchar_t* dataPath, ID3D11Device* dev, unsigned int sdkVersion,
                                                     const NVSDK_NGX_FeatureCommonInfo* info);
typedef NVSDK_NGX_Result (*PFN_D3D11_Shutdown1)(ID3D11Device* dev);
typedef NVSDK_NGX_Result (*PFN_D3D11_GetCapabilityParameters)(NVSDK_NGX_Parameter** out);
typedef NVSDK_NGX_Result (*PFN_D3D11_AllocateParameters)(NVSDK_NGX_Parameter** out);
typedef NVSDK_NGX_Result (*PFN_D3D11_DestroyParameters)(NVSDK_NGX_Parameter* p);
typedef NVSDK_NGX_Result (*PFN_D3D11_CreateFeature)(ID3D11DeviceContext* ctx, int feature, NVSDK_NGX_Parameter* p,
                                                    NVSDK_NGX_Handle** out);
typedef NVSDK_NGX_Result (*PFN_D3D11_ReleaseFeature)(NVSDK_NGX_Handle* h);
typedef NVSDK_NGX_Result (*PFN_D3D11_EvaluateFeature)(ID3D11DeviceContext* ctx, const NVSDK_NGX_Handle* h,
                                                      NVSDK_NGX_Parameter* p, void* callback);

struct NgxApi
{
    HMODULE module;
    bool isOptiScaler;
    PFN_D3D11_Init_Ext Init_Ext;
    PFN_D3D11_Init_ProjectID Init_ProjectID;
    PFN_D3D11_Shutdown1 Shutdown1;
    PFN_D3D11_GetCapabilityParameters GetCapabilityParameters;
    PFN_D3D11_AllocateParameters AllocateParameters;
    PFN_D3D11_DestroyParameters DestroyParameters;
    PFN_D3D11_CreateFeature CreateFeature;
    PFN_D3D11_ReleaseFeature ReleaseFeature;
    PFN_D3D11_EvaluateFeature EvaluateFeature;
};
extern NgxApi g_ngx;

bool NgxLoad();                         // load the NGX core (or OptiScaler) and resolve exports
bool NgxInit(ID3D11Device* dev);        // NVSDK_NGX_D3D11_Init*
bool NgxDlssAvailable(ID3D11Device* dev);
