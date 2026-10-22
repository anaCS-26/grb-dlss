// Custom internal render resolution.
// Breakpoint renders every frame at  output_size * g_ResolutionRatio  (a global float2 the engine's
// dynamic-resolution controller rewrites each frame; Temporal Injection pins it to 0.667). We patch the
// single store of the new ratio so that, when the user picked a RenderScale, our value is stored instead.
// The whole renderer (viewports, constants, TAA LowResSize) then follows, and DLSS upscales from it.
#include "common.h"
#include <tlhelp32.h>

uint8_t* AllocNear(uint8_t* addr);

static volatile float* s_ratio;   // engine global (current ratio, x == y use the same value)
static uint8_t* s_patchAt;        // the "movss [ratio], xmm6" instruction
static uint8_t* s_stub;
static volatile LONG* s_stubOn;   // inside the stub page
static volatile float* s_stubVal; // inside the stub page
static int s_state;               // 0 not tried, 1 installed, -1 failed
static float s_current;           // 0 = game default

static bool FindSite(uint8_t*& site, float*& ratio)
{
    HMODULE m = GetModuleHandleW(nullptr);
    uint8_t* base = (uint8_t*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int found = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        uint8_t* secBeg = base + sec[s].VirtualAddress;
        uint8_t* secEnd = secBeg + sec[s].Misc.VirtualSize;
        for (uint8_t* r = secBeg; r < secEnd;)
        {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(r, &mbi, sizeof(mbi)))
                break;
            uint8_t* rEnd = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
            if (rEnd > secEnd)
                rEnd = secEnd;
            bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                            (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY |
                                            PAGE_READWRITE));
            if (readable && rEnd - r > 32)
            {
                size_t n = (size_t)(rEnd - r);
                const uint8_t* p = r;
                for (size_t i = 0; i + 28 <= n; i++)
                {
                    // movss xmm0,[X] ; movss [X+4],xmm0 ; movss [X],xmm6 ; movaps xmm6,[rsp+50h]
                    const uint8_t* q = p + i;
                    if (q[0] != 0xF3 || q[1] != 0x0F || q[2] != 0x10 || q[3] != 0x05)
                        continue;
                    if (q[8] != 0xF3 || q[9] != 0x0F || q[10] != 0x11 || q[11] != 0x05)
                        continue;
                    if (q[16] != 0xF3 || q[17] != 0x0F || q[18] != 0x11 || q[19] != 0x35)
                        continue;
                    if (q[24] != 0x0F || q[25] != 0x28 || q[26] != 0x74 || q[27] != 0x24)
                        continue;
                    uintptr_t t0 = (uintptr_t)(q + 8) + *(int32_t*)(q + 4);
                    uintptr_t t1 = (uintptr_t)(q + 16) + *(int32_t*)(q + 12);
                    uintptr_t t2 = (uintptr_t)(q + 24) + *(int32_t*)(q + 20);
                    if (t1 != t0 + 4 || t2 != t0)
                        continue;
                    MEMORY_BASIC_INFORMATION m2;
                    if (!VirtualQuery((void*)t0, &m2, sizeof(m2)) || m2.State != MEM_COMMIT || !(m2.Protect & PAGE_READWRITE))
                        continue;
                    float v = *(float*)t0;
                    LOGI("RenderScale: candidate site GRB.exe+0x%I64X, ratio global at GRB.exe+0x%I64X = %.4f",
                         (uint64_t)(q + 16 - base), (uint64_t)(t0 - (uintptr_t)base), v);
                    if (!(v > 0.05f && v <= 4.0f))
                        continue;
                    site = (uint8_t*)q + 16;
                    ratio = (float*)t0;
                    found++;
                }
            }
            r = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        }
        if (found)
            break; // the engine code section comes first; don't scan the protector's sections
    }
    return found == 1;
}

static void SuspendOthers(HANDLE* hs, int& n, int cap)
{
    n = 0;
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    DWORD ids[1024];
    int ni = 0;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && ni < 1024; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self)
            ids[ni++] = te.th32ThreadID;
    CloseHandle(snap);
    for (int i = 0; i < ni && n < cap; i++)
    {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, ids[i]);
        if (!t)
            continue;
        if (SuspendThread(t) == (DWORD)-1)
        {
            CloseHandle(t);
            continue;
        }
        hs[n++] = t;
    }
}

static bool Install()
{
    uint8_t* site = nullptr;
    float* ratio = nullptr;
    if (!FindSite(site, ratio))
    {
        LOGE("RenderScale: engine resolution-ratio code not found - custom render resolution unavailable");
        return false;
    }
    uint8_t* stub = AllocNear(site);
    if (!stub)
    {
        LOGE("RenderScale: no memory near the game code for the patch stub");
        return false;
    }
    // data at stub+0x40
    volatile LONG* on = (volatile LONG*)(stub + 0x40);
    volatile float* val = (volatile float*)(stub + 0x44);
    *on = 0;
    *val = 1.0f;
    uint8_t* p = stub;
    auto rel = [](uint8_t* from_end, void* target) { return (int32_t)((intptr_t)target - (intptr_t)from_end); };
    // cmp dword [rip+on], 0
    p[0] = 0x83; p[1] = 0x3D; *(int32_t*)(p + 2) = rel(p + 7, (void*)on); p[6] = 0x00; p += 7;
    // je +8
    p[0] = 0x74; p[1] = 0x08; p += 2;
    // movss xmm6, [rip+val]
    p[0] = 0xF3; p[1] = 0x0F; p[2] = 0x10; p[3] = 0x35; *(int32_t*)(p + 4) = rel(p + 8, (void*)val); p += 8;
    // movss [rip+ratio], xmm6   (the original instruction)
    p[0] = 0xF3; p[1] = 0x0F; p[2] = 0x11; p[3] = 0x35; *(int32_t*)(p + 4) = rel(p + 8, ratio); p += 8;
    // jmp site+8
    p[0] = 0xE9; *(int32_t*)(p + 1) = rel(p + 5, site + 8); p += 5;
    FlushInstructionCache(GetCurrentProcess(), stub, 0x40);

    // The controller has a second store of the ratio (a branch that forces 1.0, e.g. while some game
    // systems request full resolution): movss [ratio], xmm7 ; jmp <after the main store>. Route it too,
    // otherwise the render resolution flips back to 100% every so often.
    uint8_t* site2 = site - 26;
    bool have2 = false;
    if (site2[0] == 0xF3 && site2[1] == 0x0F && site2[2] == 0x11 && site2[3] == 0x3D && site2[8] == 0xEB &&
        (uintptr_t)(site2 + 8) + *(int32_t*)(site2 + 4) == (uintptr_t)ratio && site2 + 10 + (int8_t)site2[9] == site + 8)
    {
        uint8_t* q2 = stub + 0x60;
        uint8_t* p2 = q2;
        p2[0] = 0x83; p2[1] = 0x3D; *(int32_t*)(p2 + 2) = rel(p2 + 7, (void*)on); p2[6] = 0x00; p2 += 7;
        p2[0] = 0x74; p2[1] = 0x08; p2 += 2;
        // movss xmm7, [rip+val]
        p2[0] = 0xF3; p2[1] = 0x0F; p2[2] = 0x10; p2[3] = 0x3D; *(int32_t*)(p2 + 4) = rel(p2 + 8, (void*)val); p2 += 8;
        // movss [rip+ratio], xmm7
        p2[0] = 0xF3; p2[1] = 0x0F; p2[2] = 0x11; p2[3] = 0x3D; *(int32_t*)(p2 + 4) = rel(p2 + 8, ratio); p2 += 8;
        // jmp site+8 (where the original short jmp went)
        p2[0] = 0xE9; *(int32_t*)(p2 + 1) = rel(p2 + 5, site + 8); p2 += 5;
        FlushInstructionCache(GetCurrentProcess(), q2, 0x40);
        have2 = true;
    }
    else
        LOGI("RenderScale: secondary ratio store not recognised (%02X %02X %02X %02X .. %02X) - only the main one is patched",
             site2[0], site2[1], site2[2], site2[3], site2[8]);

    uint8_t patch[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    *(int32_t*)(patch + 1) = rel(site + 5, stub);

    static HANDLE hs[1024];
    int n = 0;
    SuspendOthers(hs, n, 1024);
    DWORD old;
    bool ok = VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (ok)
    {
        memcpy(site, patch, 8);
        DWORD tmp;
        VirtualProtect(site, 8, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), site, 8);
        if (have2 && VirtualProtect(site2, 10, PAGE_EXECUTE_READWRITE, &old))
        {
            uint8_t patch2[10] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};
            *(int32_t*)(patch2 + 1) = rel(site2 + 5, stub + 0x60);
            memcpy(site2, patch2, 10);
            VirtualProtect(site2, 10, old, &tmp);
            FlushInstructionCache(GetCurrentProcess(), site2, 10);
        }
    }
    for (int i = 0; i < n; i++)
    {
        ResumeThread(hs[i]);
        CloseHandle(hs[i]);
    }
    if (!ok)
    {
        LOGE("RenderScale: VirtualProtect failed (%lu)", GetLastError());
        return false;
    }
    s_ratio = ratio;
    s_patchAt = site;
    s_stub = stub;
    s_stubOn = on;
    s_stubVal = val;
    LOGI("RenderScale: installed (stub %p, %d threads paused during patch, secondary store %s)", stub, n,
         have2 ? "patched" : "not patched");
    return true;
}

float RenderScaleGameRatio()
{
    return s_ratio ? s_ratio[0] : 0.0f;
}

float RenderScaleCurrent()
{
    return s_current;
}

// scale: 0 = leave the game's ratio alone, else 0.25 .. 1.0
static SRWLOCK s_lock = SRWLOCK_INIT;
void RenderScaleApply(float scale)
{
    if (s_state == -1)
        return;
    if (s_state == 1 && scale == s_current)
        return;
    AcquireSRWLockExclusive(&s_lock);
    if (s_state == 0)
        s_state = Install() ? 1 : -1;
    if (s_state != 1)
    {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    if (scale > 0.0f)
    {
        if (scale < 0.25f)
            scale = 0.25f;
        if (scale > 1.0f)
            scale = 1.0f;
    }
    else
        scale = 0.0f;
    if (scale == s_current)
    {
        ReleaseSRWLockExclusive(&s_lock);
        return;
    }
    s_current = scale;
    if (scale > 0.0f)
    {
        *s_stubVal = scale;
        MemoryBarrier();
        *s_stubOn = 1;
        LOGI("RenderScale: internal render resolution = %.0f%% of the output (game value was %.3f)", scale * 100.0f,
             s_ratio[0]);
    }
    else
    {
        *s_stubOn = 0;
        LOGI("RenderScale: off - the game controls the render resolution again");
    }
    ReleaseSRWLockExclusive(&s_lock);
}
