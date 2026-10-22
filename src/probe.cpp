// Developer probe: locates the engine's render-resolution ratio in memory (used to implement RenderScale).
// Driven by [Probe] keys in GRB_DLSS_cmd.ini (only with DevCommands=1).
#include "common.h"

static volatile float s_low[2], s_full[2], s_ratio[2], s_uvmax[2];
static volatile LONG s_haveTaa;

struct Cand
{
    uintptr_t addr;
    uint32_t val;  // value at scan time (first 4 bytes)
    uint32_t val2; // second word (pairs)
    uint8_t kind;
    uint8_t changed;
};
enum
{
    kMaxCand = 1 << 21
};
static Cand* s_cand;
static LONG s_candCount;
static volatile LONG s_busy;
static int s_scanSeq = -1, s_rescanSeq = -1, s_pokeSeq = -1;

static const char* kKindName[] = {"ratio-f32", "low-i32x2", "low-f32x2", "full-i32x2", "full-f32x2", "ratio-f32x2", "user-f32", "user-i32"};

static uintptr_t s_exeBase, s_exeEnd;

static void ExeRange()
{
    if (s_exeBase)
        return;
    HMODULE m = GetModuleHandleW(nullptr);
    s_exeBase = (uintptr_t)m;
    IMAGE_DOS_HEADER* d = (IMAGE_DOS_HEADER*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + d->e_lfanew);
    s_exeEnd = s_exeBase + nt->OptionalHeader.SizeOfImage;
}

void ProbeOnTaa(const float* low, const float* full, const float* frame16, bool haveFrame)
{
    float oldLow0 = s_low[0], oldLow1 = s_low[1], oldR = s_ratio[0], oldU = s_uvmax[0];
    s_low[0] = low[0];
    s_low[1] = low[1];
    s_full[0] = full[0];
    s_full[1] = full[1];
    if (haveFrame)
    {
        s_ratio[0] = frame16[0];
        s_ratio[1] = frame16[1];
        s_uvmax[0] = frame16[4];
        s_uvmax[1] = frame16[5];
    }
    InterlockedExchange(&s_haveTaa, 1);
    if (oldLow0 != low[0] || oldLow1 != low[1] || oldR != s_ratio[0] || oldU != s_uvmax[0])
        LOGI("Probe: TAA full %.1fx%.1f low %.1fx%.1f | g_Frame ratio (%.6f, %.6f, %.6f, %.6f) uvmax (%.6f, %.6f, %.6f, %.6f)%s",
             full[0], full[1], low[0], low[1], frame16[0], frame16[1], frame16[2], frame16[3], frame16[4], frame16[5],
             frame16[6], frame16[7], haveFrame ? "" : " (g_Frame not captured)");
}

static bool SafeRead(uintptr_t a, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (void*)a, out, n, &got) && got == n;
}
static bool SafeWrite(uintptr_t a, const void* in, size_t n)
{
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (void*)a, in, n, &put) && put == n;
}

static inline bool Near(float a, float b, float eps)
{
    float d = a - b;
    return d < eps && d > -eps;
}

struct ScanArgs
{
    float ratio;
    float lowW, lowH, fullW, fullH;
    float userF;
    int userI;
    bool haveUserF, haveUserI;
    int kinds;
};
static ScanArgs s_args;

static void AddCand(uintptr_t a, uint8_t kind, uint32_t v, uint32_t v2)
{
    if (s_candCount >= kMaxCand)
        return;
    Cand& c = s_cand[s_candCount++];
    c.addr = a;
    c.kind = kind;
    c.val = v;
    c.val2 = v2;
    c.changed = 0;
}

static DWORD WINAPI ScanThread(LPVOID)
{
    ExeRange();
    if (!s_cand)
        s_cand = (Cand*)VirtualAlloc(nullptr, sizeof(Cand) * kMaxCand, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    s_candCount = 0;
    const ScanArgs A = s_args;
    const size_t kChunk = 1 << 20;
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, kChunk + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint64_t t0 = NowQpc();
    uint64_t scanned = 0;
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mbi;
    LONG perKind[8] = {};
    while (a < 0x7FFFFFFF0000ull && VirtualQuery((void*)a, &mbi, sizeof(mbi)))
    {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        DWORD pr = mbi.Protect;
        bool rw = (pr & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE)) && !(pr & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE));
        bool own = base <= (uintptr_t)s_cand && (uintptr_t)s_cand < end;
        if (mbi.State == MEM_COMMIT && rw && !own && mbi.Type != MEM_MAPPED && (uintptr_t)buf != base)
        {
            for (uintptr_t p = base; p < end; p += kChunk)
            {
                size_t n = end - p < kChunk ? end - p : kChunk;
                if (!SafeRead(p, buf, n))
                    continue;
                scanned += n;
                for (size_t i = 0; i + 8 <= n; i += 4)
                {
                    float f0, f1;
                    uint32_t u0, u1;
                    memcpy(&f0, buf + i, 4);
                    memcpy(&f1, buf + i + 4, 4);
                    memcpy(&u0, buf + i, 4);
                    memcpy(&u1, buf + i + 4, 4);
                    if (A.ratio < 0.999f && (A.kinds & 0x21) && Near(f0, A.ratio, 2e-4f))
                    {
                        if (Near(f1, A.ratio, 2e-4f))
                            AddCand(p + i, 5, u0, u1), perKind[5]++;
                        else
                            AddCand(p + i, 0, u0, u1), perKind[0]++;
                    }
                    if ((int)u0 == (int)A.lowW && (int)u1 == (int)A.lowH)
                        AddCand(p + i, 1, u0, u1), perKind[1]++;
                    if (f0 == A.lowW && f1 == A.lowH)
                        AddCand(p + i, 2, u0, u1), perKind[2]++;
                    if ((A.fullW != A.lowW) && (int)u0 == (int)A.fullW && (int)u1 == (int)A.fullH)
                        AddCand(p + i, 3, u0, u1), perKind[3]++;
                    if ((A.fullW != A.lowW) && f0 == A.fullW && f1 == A.fullH)
                        AddCand(p + i, 4, u0, u1), perKind[4]++;
                    if (A.haveUserF && Near(f0, A.userF, 1e-5f))
                        AddCand(p + i, 6, u0, u1), perKind[6]++;
                    if (A.haveUserI && (int)u0 == A.userI)
                        AddCand(p + i, 7, u0, u1), perKind[7]++;
                }
            }
        }
        a = end;
    }
    LOGI("Probe scan: %.0f MB in %.0f ms, %ld candidates (ratio %.6f low %.0fx%.0f full %.0fx%.0f)",
         scanned / 1048576.0, QpcToMs(NowQpc() - t0), s_candCount, A.ratio, A.lowW, A.lowH, A.fullW, A.fullH);
    for (int k = 0; k < 8; k++)
        if (perKind[k])
            LOGI("Probe scan:   %-12s %ld", kKindName[k], perKind[k]);
    // list the small categories in full
    int logged = 0;
    for (LONG i = 0; i < s_candCount && logged < 400; i++)
    {
        Cand& c = s_cand[i];
        if (c.kind == 0 && perKind[0] > 300)
            continue;
        if (c.kind >= 6 && perKind[c.kind] > 300)
            continue;
        bool inExe = c.addr >= s_exeBase && c.addr < s_exeEnd;
        LOGI("Probe   #%ld %-12s %p %s+0x%I64X val %08X %08X", i, kKindName[c.kind], (void*)c.addr, inExe ? "GRB.exe" : "heap",
             (uint64_t)(inExe ? c.addr - s_exeBase : 0), c.val, c.val2);
        logged++;
    }
    VirtualFree(buf, 0, MEM_RELEASE);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

static DWORD WINAPI RescanThread(LPVOID)
{
    LONG nChanged = 0, nGone = 0;
    LONG perKind[8] = {};
    for (LONG i = 0; i < s_candCount; i++)
    {
        Cand& c = s_cand[i];
        uint32_t v[2];
        if (!SafeRead(c.addr, v, 8))
        {
            nGone++;
            c.changed = 0;
            continue;
        }
        bool ch = v[0] != c.val || ((c.kind != 0 && c.kind < 6) && v[1] != c.val2);
        c.changed = ch ? 1 : 0;
        if (ch)
        {
            nChanged++;
            perKind[c.kind]++;
            if (nChanged <= 600)
            {
                float fo, fn;
                memcpy(&fo, &c.val, 4);
                memcpy(&fn, &v[0], 4);
                bool inExe = c.addr >= s_exeBase && c.addr < s_exeEnd;
                LOGI("Probe   chg #%ld %-12s %p %s+0x%I64X  %08X %08X -> %08X %08X  (f %.6f -> %.6f, i %d,%d -> %d,%d)", i,
                     kKindName[c.kind], (void*)c.addr, inExe ? "GRB.exe" : "heap", (uint64_t)(inExe ? c.addr - s_exeBase : 0),
                     c.val, c.val2, v[0], v[1], fo, fn, (int)c.val, (int)c.val2, (int)v[0], (int)v[1]);
            }
        }
    }
    LOGI("Probe rescan: %ld of %ld changed, %ld unreadable", nChanged, s_candCount, nGone);
    for (int k = 0; k < 8; k++)
        if (perKind[k])
            LOGI("Probe rescan:   %-12s %ld changed", kKindName[k], perKind[k]);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

static float WToF(const wchar_t* b)
{
    double sign = 1, v = 0, frac = 0.1;
    int i = 0;
    if (b[0] == L'-')
        sign = -1, i = 1;
    for (; b[i] >= L'0' && b[i] <= L'9'; i++)
        v = v * 10 + (b[i] - L'0');
    if (b[i] == L'.')
        for (i++; b[i] >= L'0' && b[i] <= L'9'; i++, frac *= 0.1)
            v += (b[i] - L'0') * frac;
    return (float)(v * sign);
}

// [Probe] Scan=n  Rescan=n  Poke=n PokeIndex=i (or -1 = all changed of PokeKind) PokeKind=k PokeF=x | PokeI=x PokeI2=y
void ProbePoll2(const wchar_t* ini);
void ProbePoll(const wchar_t* ini)
{
    ProbePoll2(ini);
    if (s_busy)
        return;
    int scan = GetPrivateProfileIntW(L"Probe", L"Scan", -1, ini);
    int rescan = GetPrivateProfileIntW(L"Probe", L"Rescan", -1, ini);
    int poke = GetPrivateProfileIntW(L"Probe", L"Poke", -1, ini);
    if (s_scanSeq == -1 && s_rescanSeq == -1 && s_pokeSeq == -1)
    {
        // first poll: remember current values, act only on changes
        s_scanSeq = scan;
        s_rescanSeq = rescan;
        s_pokeSeq = poke;
        if (scan == -1 && rescan == -1 && poke == -1)
            s_scanSeq = -2;
        return;
    }
    wchar_t b[64];
    if (scan != s_scanSeq && scan >= 0 && s_haveTaa)
    {
        s_scanSeq = scan;
        s_args.ratio = s_low[0] / s_full[0];
        s_args.lowW = s_low[0];
        s_args.lowH = s_low[1];
        s_args.fullW = s_full[0];
        s_args.fullH = s_full[1];
        s_args.kinds = GetPrivateProfileIntW(L"Probe", L"ScanKinds", 0xFF, ini);
        GetPrivateProfileStringW(L"Probe", L"UserF", L"", b, 64, ini);
        s_args.haveUserF = b[0] != 0;
        s_args.userF = b[0] ? WToF(b) : 0;
        GetPrivateProfileStringW(L"Probe", L"UserI", L"", b, 64, ini);
        s_args.haveUserI = b[0] != 0;
        s_args.userI = b[0] ? (int)WToF(b) : 0;
        InterlockedExchange(&s_busy, 1);
        HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            s_busy = 0;
        return;
    }
    if (rescan != s_rescanSeq && rescan >= 0 && s_cand)
    {
        s_rescanSeq = rescan;
        InterlockedExchange(&s_busy, 1);
        HANDLE t = CreateThread(nullptr, 0, RescanThread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            s_busy = 0;
        return;
    }
    if (poke != s_pokeSeq && poke >= 0 && s_cand)
    {
        s_pokeSeq = poke;
        int idx = GetPrivateProfileIntW(L"Probe", L"PokeIndex", -2, ini);
        int kind = GetPrivateProfileIntW(L"Probe", L"PokeKind", -1, ini);
        uint32_t w[2];
        int words = 1;
        GetPrivateProfileStringW(L"Probe", L"PokeF", L"", b, 64, ini);
        if (b[0])
        {
            float f = WToF(b);
            memcpy(&w[0], &f, 4);
            memcpy(&w[1], &f, 4);
            GetPrivateProfileStringW(L"Probe", L"PokeF2", L"", b, 64, ini);
            if (b[0])
            {
                f = WToF(b);
                memcpy(&w[1], &f, 4);
                words = 2;
            }
        }
        else
        {
            w[0] = (uint32_t)GetPrivateProfileIntW(L"Probe", L"PokeI", 0, ini);
            w[1] = (uint32_t)GetPrivateProfileIntW(L"Probe", L"PokeI2", 0, ini);
            words = 2;
        }
        int pairWords = GetPrivateProfileIntW(L"Probe", L"PokeWords", words, ini);
        int n = 0;
        for (LONG i = 0; i < s_candCount; i++)
        {
            Cand& c = s_cand[i];
            bool sel = idx >= 0 ? i == idx : (idx == -1 && c.changed && (kind < 0 || c.kind == kind));
            if (!sel)
                continue;
            bool ok = SafeWrite(c.addr, w, 4 * pairWords);
            LOGI("Probe poke #%ld %p <- %08X %08X (%d words) %s", i, (void*)c.addr, w[0], w[1], pairWords, ok ? "ok" : "FAILED");
            n++;
        }
        LOGI("Probe poke: %d addresses written", n);
    }
}

// ------------------------------------------------------------------ viewport callers
struct VpRec
{
    uintptr_t ret;
    float w, h, x, y;
    LONG count;
};
static VpRec s_vp[512];
static volatile LONG s_vpCount;
static volatile LONG s_vpActive;
static uint64_t s_vpEnd;
static SRWLOCK s_vpLock = SRWLOCK_INIT;

void ProbeViewport(void* ret, ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* vp)
{
    if (!s_vpActive || !n || !vp)
        return;
    if (NowQpc() > s_vpEnd)
    {
        if (InterlockedExchange(&s_vpActive, 0))
        {
            ExeRange();
            AcquireSRWLockExclusive(&s_vpLock);
            LOGI("Probe viewports: %ld distinct (caller, size)", s_vpCount);
            for (LONG i = 0; i < s_vpCount; i++)
            {
                VpRec& r = s_vp[i];
                bool inExe = r.ret >= s_exeBase && r.ret < s_exeEnd;
                LOGI("Probe   vp %s+0x%I64X  %.0fx%.0f @%.0f,%.0f  x%ld", inExe ? "GRB.exe" : "other",
                     (uint64_t)(inExe ? r.ret - s_exeBase : r.ret), r.w, r.h, r.x, r.y, r.count);
            }
            ReleaseSRWLockExclusive(&s_vpLock);
        }
        return;
    }
    AcquireSRWLockExclusive(&s_vpLock);
    LONG i;
    for (i = 0; i < s_vpCount; i++)
        if (s_vp[i].ret == (uintptr_t)ret && s_vp[i].w == vp->Width && s_vp[i].h == vp->Height)
            break;
    if (i < s_vpCount)
        s_vp[i].count++;
    else if (s_vpCount < 512)
    {
        VpRec& r = s_vp[s_vpCount++];
        r.ret = (uintptr_t)ret;
        r.w = vp->Width;
        r.h = vp->Height;
        r.x = vp->TopLeftX;
        r.y = vp->TopLeftY;
        r.count = 1;
    }
    ReleaseSRWLockExclusive(&s_vpLock);
}

// ------------------------------------------------------------------ image dump
static void DumpImage()
{
    ExeRange();
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_modDir);
    lstrcatW(p, L"GRB_memdump.bin");
    HANDLE f = CreateFileW(p, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    const size_t kPage = 1 << 16;
    uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    size_t bad = 0;
    for (uintptr_t a = s_exeBase; a < s_exeEnd; a += kPage)
    {
        size_t n = s_exeEnd - a < kPage ? s_exeEnd - a : kPage;
        if (!SafeRead(a, buf, n))
        {
            // page by page
            for (size_t o = 0; o < n; o += 4096)
                if (!SafeRead(a + o, buf + o, 4096))
                {
                    memset(buf + o, 0, 4096);
                    bad++;
                }
        }
        DWORD w;
        WriteFile(f, buf, (DWORD)n, &w, nullptr);
    }
    CloseHandle(f);
    VirtualFree(buf, 0, MEM_RELEASE);
    LOGI("Probe: dumped GRB.exe image base %p size 0x%I64X (%Iu unreadable pages)", (void*)s_exeBase,
         (uint64_t)(s_exeEnd - s_exeBase), bad);
}
static DWORD WINAPI DumpThread(LPVOID)
{
    DumpImage();
    InterlockedExchange(&s_busy, 0);
    return 0;
}

// ------------------------------------------------------------------ hardware write watchpoints
#include <tlhelp32.h>
struct WpHit
{
    uintptr_t rip;
    uint64_t dr6;
    LONG count;
    uint64_t regs[16];
};
static WpHit s_hits[64];
static volatile LONG s_hitCount;
static volatile LONG s_wpOn;
static uintptr_t s_wpAddr[4];
static PVOID s_veh;

static LONG CALLBACK WpHandler(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    if (!(c->Dr6 & 0xF))
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)c->Rip;
    LONG n = s_hitCount;
    LONG i;
    for (i = 0; i < n; i++)
        if (s_hits[i].rip == rip)
        {
            InterlockedIncrement(&s_hits[i].count);
            break;
        }
    if (i == n && n < 64)
    {
        LONG k = InterlockedIncrement(&s_hitCount) - 1;
        if (k < 64)
        {
            WpHit& h = s_hits[k];
            h.dr6 = c->Dr6;
            h.count = 1;
            uint64_t* r = h.regs;
            r[0] = c->Rax; r[1] = c->Rcx; r[2] = c->Rdx; r[3] = c->Rbx; r[4] = c->Rsp; r[5] = c->Rbp; r[6] = c->Rsi; r[7] = c->Rdi;
            r[8] = c->R8; r[9] = c->R9; r[10] = c->R10; r[11] = c->R11; r[12] = c->R12; r[13] = c->R13; r[14] = c->R14; r[15] = c->R15;
            MemoryBarrier();
            h.rip = rip;
        }
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void SetDrAllThreads(bool on)
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int nt = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t)
            continue;
        if (SuspendThread(t) != (DWORD)-1)
        {
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c))
            {
                uint64_t dr7 = 0;
                if (on)
                {
                    c.Dr0 = s_wpAddr[0]; c.Dr1 = s_wpAddr[1]; c.Dr2 = s_wpAddr[2]; c.Dr3 = s_wpAddr[3];
                    for (int k = 0; k < 4; k++)
                        if (s_wpAddr[k])
                            dr7 |= (1ull << (2 * k)) | (1ull << (16 + 4 * k)) /*RW=01 write*/ | (3ull << (18 + 4 * k)) /*len 4*/;
                }
                else
                    c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0;
                c.Dr7 = dr7;
                c.Dr6 = 0;
                if (SetThreadContext(t, &c))
                    nt++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    LOGT("Probe: debug registers %s on %d threads", on ? "armed" : "cleared", nt);
}

static DWORD WINAPI WatchThread(LPVOID p)
{
    DWORD ms = (DWORD)(uintptr_t)p;
    ExeRange();
    s_hitCount = 0;
    memset(s_hits, 0, sizeof(s_hits));
    if (!s_veh)
        s_veh = AddVectoredExceptionHandler(1, WpHandler);
    SetDrAllThreads(true);
    Sleep(ms);
    SetDrAllThreads(false);
    LONG n = s_hitCount > 64 ? 64 : s_hitCount;
    LOGI("Probe watch: %ld distinct writer sites (addr %p %p %p %p)", n, (void*)s_wpAddr[0], (void*)s_wpAddr[1],
         (void*)s_wpAddr[2], (void*)s_wpAddr[3]);
    for (LONG i = 0; i < n; i++)
    {
        WpHit& h = s_hits[i];
        bool inExe = h.rip >= s_exeBase && h.rip < s_exeEnd;
        LOGI("Probe   wp %s+0x%I64X (rip %p) dr6 %I64X x%ld", inExe ? "GRB.exe" : "other",
             (uint64_t)(inExe ? h.rip - s_exeBase : h.rip), (void*)h.rip, h.dr6, h.count);
        LOGI("Probe      rax %I64X rcx %I64X rdx %I64X rbx %I64X rsp %I64X rbp %I64X rsi %I64X rdi %I64X", h.regs[0], h.regs[1],
             h.regs[2], h.regs[3], h.regs[4], h.regs[5], h.regs[6], h.regs[7]);
        LOGI("Probe      r8 %I64X r9 %I64X r10 %I64X r11 %I64X r12 %I64X r13 %I64X r14 %I64X r15 %I64X", h.regs[8], h.regs[9],
             h.regs[10], h.regs[11], h.regs[12], h.regs[13], h.regs[14], h.regs[15]);
        // stack: a few return-address-looking values
        uint64_t stk[48];
        if (SafeRead((uintptr_t)h.regs[4], stk, sizeof(stk)))
        {
            char line[1024];
            int o = 0;
            for (int k = 0; k < 48 && o < 900; k++)
                if (stk[k] >= s_exeBase && stk[k] < s_exeEnd)
                    o += _snprintf(line + o, sizeof(line) - o, " [%d]+0x%I64X", k, (uint64_t)(stk[k] - s_exeBase));
            line[o] = 0;
            LOGI("Probe      stack:%s", line);
        }
    }
    InterlockedExchange(&s_busy, 0);
    return 0;
}

struct AggHit
{
    uintptr_t rip;
    uintptr_t sampleAddr;
    LONG count, cands;
    uint64_t regs[16];
    uint64_t stack[8];
};
static AggHit s_agg[256];
static LONG s_aggCount;
static int s_batchKinds = 0x6; // bit per kind
static DWORD s_batchMs = 250;

static DWORD WINAPI BatchWatchThread(LPVOID)
{
    ExeRange();
    if (!s_veh)
        s_veh = AddVectoredExceptionHandler(1, WpHandler);
    s_aggCount = 0;
    LONG total = 0, batches = 0;
    LONG i = 0;
    while (i < s_candCount)
    {
        int k = 0;
        memset(s_wpAddr, 0, sizeof(s_wpAddr));
        while (k < 4 && i < s_candCount)
        {
            Cand& c = s_cand[i++];
            if (!((s_batchKinds >> c.kind) & 1))
                continue;
            if (c.addr >= (uintptr_t)s_cand && c.addr < (uintptr_t)s_cand + sizeof(Cand) * kMaxCand)
                continue;
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)c.addr, &self);
            if (self && self != GetModuleHandleW(nullptr))
                continue; // inside some other module (ours, drivers...)
            s_wpAddr[k++] = c.addr & ~(uintptr_t)3;
            total++;
        }
        if (!k)
            break;
        s_hitCount = 0;
        memset(s_hits, 0, sizeof(s_hits));
        SetDrAllThreads(true);
        Sleep(s_batchMs);
        SetDrAllThreads(false);
        batches++;
        LONG n = s_hitCount > 64 ? 64 : s_hitCount;
        for (LONG h = 0; h < n; h++)
        {
            WpHit& w = s_hits[h];
            if (!w.rip)
                continue;
            LONG a;
            for (a = 0; a < s_aggCount; a++)
                if (s_agg[a].rip == w.rip)
                    break;
            if (a == s_aggCount && s_aggCount < 256)
            {
                AggHit& g = s_agg[s_aggCount++];
                g.rip = w.rip;
                g.count = 0;
                g.cands = 0;
                memcpy(g.regs, w.regs, sizeof(g.regs));
                // which watched address: dr6 bits
                int bit = (w.dr6 & 1) ? 0 : (w.dr6 & 2) ? 1 : (w.dr6 & 4) ? 2 : 3;
                g.sampleAddr = s_wpAddr[bit];
                uint64_t stk[64];
                int o = 0;
                memset(g.stack, 0, sizeof(g.stack));
                if (SafeRead((uintptr_t)w.regs[4], stk, sizeof(stk)))
                    for (int q = 0; q < 64 && o < 8; q++)
                        if (stk[q] >= s_exeBase && stk[q] < s_exeEnd)
                            g.stack[o++] = stk[q] - s_exeBase;
            }
            if (a < 256)
            {
                s_agg[a].count += w.count;
                s_agg[a].cands++;
            }
        }
    }
    LOGI("Probe batch watch: %ld addresses in %ld batches, %ld distinct writer sites", total, batches, s_aggCount);
    for (LONG a = 0; a < s_aggCount; a++)
    {
        AggHit& g = s_agg[a];
        bool inExe = g.rip >= s_exeBase && g.rip < s_exeEnd;
        uint32_t v[2] = {};
        SafeRead(g.sampleAddr, v, 8);
        LOGI("Probe   bw %s+0x%I64X hits %ld cands %ld sample %p (=%u,%u) rcx %I64X rdx %I64X rbx %I64X rsi %I64X rdi %I64X r8 %I64X",
             inExe ? "GRB.exe" : "other", (uint64_t)(inExe ? g.rip - s_exeBase : g.rip), g.count, g.cands, (void*)g.sampleAddr,
             v[0], v[1], g.regs[1], g.regs[2], g.regs[3], g.regs[6], g.regs[7], g.regs[8]);
        LOGI("Probe      stack +%I64X +%I64X +%I64X +%I64X +%I64X +%I64X +%I64X +%I64X", g.stack[0], g.stack[1], g.stack[2],
             g.stack[3], g.stack[4], g.stack[5], g.stack[6], g.stack[7]);
    }
    InterlockedExchange(&s_busy, 0);
    return 0;
}

static uintptr_t WToHex(const wchar_t* b)
{
    uintptr_t v = 0;
    int i = 0;
    if (b[0] == L'0' && (b[1] == L'x' || b[1] == L'X'))
        i = 2;
    for (; b[i]; i++)
    {
        wchar_t ch = b[i];
        int d = (ch >= L'0' && ch <= L'9') ? ch - L'0' : (ch >= L'a' && ch <= L'f') ? ch - L'a' + 10 : (ch >= L'A' && ch <= L'F') ? ch - L'A' + 10 : -1;
        if (d < 0)
            break;
        v = v * 16 + d;
    }
    return v;
}

// [Probe] Viewports=n  Dump=n  Watch=n WatchAddr0..3=hex WatchMs=3000  WatchCand0..3=index
static int s_vpSeq = -1, s_dumpSeq = -1, s_watchSeq = -1, s_batchSeq = -1;
static bool s_extInit;
void ProbePoll2(const wchar_t* ini)
{
    int vps = GetPrivateProfileIntW(L"Probe", L"Viewports", -1, ini);
    int dump = GetPrivateProfileIntW(L"Probe", L"Dump", -1, ini);
    int watch = GetPrivateProfileIntW(L"Probe", L"Watch", -1, ini);
    int batch = GetPrivateProfileIntW(L"Probe", L"BatchWatch", -1, ini);
    if (!s_extInit)
    {
        s_extInit = true;
        s_batchSeq = batch;
        s_vpSeq = vps;
        s_dumpSeq = dump;
        s_watchSeq = watch;
        return;
    }
    if (vps != s_vpSeq && vps >= 0)
    {
        s_vpSeq = vps;
        AcquireSRWLockExclusive(&s_vpLock);
        s_vpCount = 0;
        ReleaseSRWLockExclusive(&s_vpLock);
        s_vpEnd = NowQpc() + (uint64_t)(1000.0 / QpcToMs(1)); // ~1 s
        InterlockedExchange(&s_vpActive, 1);
    }
    if (s_busy)
        return;
    if (dump != s_dumpSeq && dump >= 0)
    {
        s_dumpSeq = dump;
        InterlockedExchange(&s_busy, 1);
        HANDLE t = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            s_busy = 0;
        return;
    }
    if (batch != s_batchSeq && batch >= 0 && s_cand)
    {
        s_batchSeq = batch;
        s_batchKinds = GetPrivateProfileIntW(L"Probe", L"BatchKinds", 0x6, ini);
        s_batchMs = GetPrivateProfileIntW(L"Probe", L"BatchMs", 250, ini);
        InterlockedExchange(&s_busy, 1);
        HANDLE t = CreateThread(nullptr, 0, BatchWatchThread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            s_busy = 0;
        return;
    }
    if (watch != s_watchSeq && watch >= 0)
    {
        s_watchSeq = watch;
        wchar_t b[64];
        for (int k = 0; k < 4; k++)
        {
            wchar_t key[16] = L"WatchAddr0";
            key[9] = L'0' + k;
            GetPrivateProfileStringW(L"Probe", key, L"", b, 64, ini);
            s_wpAddr[k] = b[0] ? WToHex(b) & ~(uintptr_t)3 : 0;
            wchar_t key2[16] = L"WatchCand0";
            key2[9] = L'0' + k;
            int ci = GetPrivateProfileIntW(L"Probe", key2, -1, ini);
            if (k == 0 && GetPrivateProfileIntW(L"Probe", L"WatchSelf", 0, ini))
                s_wpAddr[0] = (uintptr_t)&s_low[0];
            if (ci >= 0 && ci < s_candCount)
                s_wpAddr[k] = s_cand[ci].addr & ~(uintptr_t)3;
        }
        int ms = GetPrivateProfileIntW(L"Probe", L"WatchMs", 2000, ini);
        InterlockedExchange(&s_busy, 1);
        HANDLE t = CreateThread(nullptr, 0, WatchThread, (LPVOID)(uintptr_t)ms, 0, nullptr);
        if (t)
            CloseHandle(t);
        else
            s_busy = 0;
    }
}
