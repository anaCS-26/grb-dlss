// Real camera data for frame generation.
//
// When OptiScaler drives frame generation from the upscaler (the only way in a DX11 game), it fills
// Streamline's sl::Constants with placeholder camera matrices (identity clip->prev-clip, a synthetic
// projection). DLSS-G uses those matrices to reproject the background it has to reveal next to moving
// foreground objects, so with placeholders the area around the player character breaks up.
//
// We know the game's real matrices (cb_g_Pass, the TAA reprojection). This module:
//  1. builds Streamline-convention matrices from them every frame, auto-calibrating the game's
//     conventions (camera-relative translation, jitter) against the TAA's own reprojection matrix;
//  2. hooks slSetConstants in OptiScaler's private Streamline (export-table patch when
//     sl.interposer.dll loads) and replaces the placeholder camera fields before they reach DLSS-G.
#include "common.h"

// ---------------------------------------------------------------- tiny 4x4 maths (column vectors: out = M * v)
struct M4
{
    double m[4][4];
};
static M4 Ident()
{
    M4 r = {};
    for (int i = 0; i < 4; i++)
        r.m[i][i] = 1.0;
    return r;
}
static M4 FromFloats(const float* f) // memory rows = matrix rows (the game's HLSL uses dot(v, M[row]))
{
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = f[i * 4 + j];
    return r;
}
static M4 Mul(const M4& a, const M4& b)
{
    M4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            double s = 0;
            for (int k = 0; k < 4; k++)
                s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
static bool Inverse(const M4& in, M4& out)
{
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
        {
            a[i][j] = in.m[i][j];
            a[i][j + 4] = i == j ? 1.0 : 0.0;
        }
    for (int c = 0; c < 4; c++)
    {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) > fabs(a[p][c]))
                p = r;
        if (fabs(a[p][c]) < 1e-30)
            return false;
        if (p != c)
            for (int j = 0; j < 8; j++)
            {
                double t = a[c][j];
                a[c][j] = a[p][j];
                a[p][j] = t;
            }
        double d = a[c][c];
        for (int j = 0; j < 8; j++)
            a[c][j] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c)
            {
                double f = a[r][c];
                if (f != 0.0)
                    for (int j = 0; j < 8; j++)
                        a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out.m[i][j] = a[i][j + 4];
    return true;
}
static void Apply(const M4& m, const double v[4], double o[4])
{
    for (int i = 0; i < 4; i++)
        o[i] = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] + m.m[i][3] * v[3];
}
static M4 Translate(double x, double y, double z)
{
    M4 r = Ident();
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}
static M4 Jitter(double jx, double jy) // clip' = J * clip : x += jx*w, y += jy*w
{
    M4 r = Ident();
    r.m[0][3] = jx;
    r.m[1][3] = jy;
    return r;
}
// Streamline wants row-major storage with row vectors (v * M), i.e. the transpose of ours.
static void ToSL(const M4& m, float* out16)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out16[i * 4 + j] = (float)m.m[j][i];
}

// ---------------------------------------------------------------- per-frame camera state
struct CamOut
{
    float viewToClip[16], clipToView[16], clipToPrev[16], prevToClip[16];
    float pos[3], up[3], right[3], fwd[3];
    uint64_t qpc;
    bool valid;
};
static CamOut s_cam;
static SRWLOCK s_camLock = SRWLOCK_INIT;
static volatile LONG s_fitLogged, s_dumped, s_badFits;
static double s_lastFitErr = -1;

static void Pass(const uint8_t* pass, int off, float* out, int n)
{
    memcpy(out, pass + off, n * 4);
}

// Smallest-eigenvalue eigenvector of a symmetric 16x16 matrix (cyclic Jacobi).
static void SmallestEigen(double a[16][16], double out[16])
{
    double v[16][16];
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            v[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 60; sweep++)
    {
        double off = 0;
        for (int p = 0; p < 16; p++)
            for (int q = p + 1; q < 16; q++)
                off += a[p][q] * a[p][q];
        if (off < 1e-30)
            break;
        for (int p = 0; p < 16; p++)
            for (int q = p + 1; q < 16; q++)
            {
                if (fabs(a[p][q]) < 1e-300)
                    continue;
                double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                double c = 1.0 / sqrt(t * t + 1.0), sn = t * c;
                for (int k = 0; k < 16; k++)
                {
                    double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - sn * akq;
                    a[k][q] = sn * akp + c * akq;
                }
                for (int k = 0; k < 16; k++)
                {
                    double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - sn * aqk;
                    a[q][k] = sn * apk + c * aqk;
                }
                for (int k = 0; k < 16; k++)
                {
                    double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - sn * vkq;
                    v[k][q] = sn * vkp + c * vkq;
                }
            }
    }
    int best = 0;
    for (int i = 1; i < 16; i++)
        if (a[i][i] < a[best][best])
            best = i;
    for (int i = 0; i < 16; i++)
        out[i] = v[i][best];
}

// The TAA's reprojection (verified against the image) maps a pixel (ndc.xy) at depth d to the previous frame:
// prev = R * (ndc.x, ndc.y, ViewZ(d), 1). Streamline wants the same mapping as one 4x4 on clip space
// (ndc.x, ndc.y, d, 1). For a static world that mapping is exactly projective, so fit it (DLT) to samples of R.
static bool FitClipToPrev(const M4& R, const float* cz, const float* low, M4& C, double& errPx)
{
    static double ata[16][16];
    memset(ata, 0, sizeof(ata));
    static const double dd[6] = {0.0002, 0.001, 0.004, 0.015, 0.06, 0.25};
    int npts = 0;
    for (int ix = 0; ix < 5; ix++)
        for (int iy = 0; iy < 5; iy++)
            for (int id = 0; id < 6; id++)
            {
                double x = -0.9 + 0.45 * ix, y = -0.9 + 0.45 * iy, d = dd[id];
                double den = cz[2] * d - cz[3];
                if (fabs(den) < 1e-20)
                    continue;
                double vz = (-cz[0] * d + cz[1]) / den;
                double in[4] = {x, y, vz, 1.0}, t[4];
                Apply(R, in, t);
                double tn = sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[3] * t[3]);
                if (!(tn > 1e-20))
                    continue;
                for (int k = 0; k < 4; k++)
                    t[k] /= tn;
                double q[4] = {x, y, d, 1.0};
                // equations: (C_i . q) * t_3 - (C_3 . q) * t_i = 0, i = 0..2
                for (int i = 0; i < 3; i++)
                {
                    double row[16] = {};
                    for (int k = 0; k < 4; k++)
                    {
                        row[i * 4 + k] = q[k] * t[3];
                        row[12 + k] = -q[k] * t[i];
                    }
                    for (int r = 0; r < 16; r++)
                        if (row[r] != 0.0)
                            for (int c = 0; c < 16; c++)
                                ata[r][c] += row[r] * row[c];
                }
                npts++;
            }
    if (npts < 50)
        return false;
    double c[16];
    SmallestEigen(ata, c);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            C.m[i][j] = c[i * 4 + j];
    // residual in pixels
    double e = 0;
    int n = 0;
    for (int ix = 0; ix < 5; ix++)
        for (int iy = 0; iy < 5; iy++)
            for (int id = 0; id < 6; id++)
            {
                double x = -0.9 + 0.45 * ix, y = -0.9 + 0.45 * iy, d = dd[id];
                double den = cz[2] * d - cz[3];
                if (fabs(den) < 1e-20)
                    continue;
                double vz = (-cz[0] * d + cz[1]) / den;
                double in[4] = {x, y, vz, 1.0}, t[4], pc[4], q[4] = {x, y, d, 1.0};
                Apply(R, in, t);
                Apply(C, q, pc);
                if (fabs(t[3]) < 1e-20 || fabs(pc[3]) < 1e-20)
                    continue;
                double ex = (pc[0] / pc[3] - t[0] / t[3]) * low[0] * 0.5, ey = (pc[1] / pc[3] - t[1] / t[3]) * low[1] * 0.5;
                e += sqrt(ex * ex + ey * ey);
                n++;
            }
    errPx = n ? e / n : 1e9;
    // keep w positive-ish for points in front of the camera
    double q0[4] = {0, 0, 0.01, 1}, w0[4];
    Apply(C, q0, w0);
    if (w0[3] < 0)
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                C.m[i][j] = -C.m[i][j];
    return errPx < 1.0;
}

static uint64_t s_lastDump;
static void DumpOnce(const uint8_t* pass, const float* reproj, const float* cz)
{
    uint64_t now = NowQpc();
    if (s_lastDump && QpcToMs(now - s_lastDump) < 3000.0)
        return;
    s_lastDump = now;
    if (InterlockedIncrement(&s_dumped) > 40)
        return;
    float f[16];
    LOGI("FG camera dump: clipZToViewZ %.6g %.6g %.6g %.6g", cz[0], cz[1], cz[2], cz[3]);
    LOGI("FG camera dump: Reproj r0 %.6g %.6g %.6g %.6g | r1 %.6g %.6g %.6g %.6g | r2 %.6g %.6g %.6g %.6g | r3 %.6g %.6g %.6g %.6g",
         reproj[0], reproj[1], reproj[2], reproj[3], reproj[4], reproj[5], reproj[6], reproj[7], reproj[8], reproj[9],
         reproj[10], reproj[11], reproj[12], reproj[13], reproj[14], reproj[15]);
    static const int offs[5] = {640, 704, 512, 768, 576};
    static const char* nm[5] = {"Proj", "ViewProj", "PrevViewProj", "ViewNoTransProj", "PrevViewNoTransProj"};
    for (int k = 0; k < 5; k++)
    {
        Pass(pass, offs[k], f, 16);
        LOGI("FG camera dump: %-20s r0 %.5g %.5g %.5g %.5g | r1 %.5g %.5g %.5g %.5g | r2 %.5g %.5g %.5g %.5g | r3 %.5g %.5g %.5g %.5g",
             nm[k], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    float e[4];
    Pass(pass, 352, e, 4);
    LOGI("FG camera dump: eye %.3f %.3f %.3f", e[0], e[1], e[2]);
    Pass(pass, 368, e, 4);
    LOGI("FG camera dump: eyeDir %.4f %.4f %.4f", e[0], e[1], e[2]);
    Pass(pass, 384, f, 16);
    LOGI("FG camera dump: viewToWorld r0 %.4f %.4f %.4f %.4f | r1 %.4f %.4f %.4f %.4f | r2 %.4f %.4f %.4f %.4f", f[0], f[1],
         f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
}

// pass: cb_g_Pass bytes 0..863; reproj: TAA ReprojectionMatrix (16 floats); jitter: SubPixelJitter (render pixels);
// low: LowResSize
static bool s_havePrevProj;
static M4 s_prevProj;
static bool Finite(const M4& m)
{
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            if (!(fabs(m.m[i][j]) < 1e30))
                return false;
    return true;
}
void SlCamUpdate(const uint8_t* pass, const float* reproj, const float* jitter, const float* low)
{
    float cz[4], f[16];
    Pass(pass, 320, cz, 4);
    M4 R = FromFloats(reproj);
    {
        double in[4] = {0.5, 0.0, (-cz[0] * 0.005 + cz[1]) / (cz[2] * 0.005 - cz[3]), 1.0}, t[4];
        Apply(R, in, t);
        if (fabs(t[3]) > 1e-12 && fabs(t[0] / t[3] - 0.5) * low[0] * 0.5 > 3.0)
            DumpOnce(pass, reproj, cz);
    }

    Pass(pass, 640, f, 16);
    M4 Pc = FromFloats(f); // projection, carries this frame's TAA jitter in the z column
    Pass(pass, 704, f, 16);
    M4 VP = FromFloats(f);
    Pass(pass, 512, f, 16);
    M4 PVP = FromFloats(f);
    float eye[4], dir[4], vtw[16];
    Pass(pass, 352, eye, 4);
    Pass(pass, 368, dir, 4);
    Pass(pass, 384, vtw, 16);

    M4 Pprev = s_havePrevProj ? s_prevProj : Pc;
    s_prevProj = Pc;
    s_havePrevProj = true;

    bool ok = true;
    const char* why = "";
    // loading screens / menus put a placeholder camera at the origin
    if (eye[0] == 0.0f && eye[1] == 0.0f && eye[2] == 0.0f)
        ok = false, why = "placeholder camera";
    M4 Pu = Pc;
    Pu.m[0][2] = 0.0;
    Pu.m[1][2] = 0.0;
    M4 iPc, iPprev, iVPu, C, Cinv, Pinv;
    if (ok && !(Inverse(Pc, iPc) && Inverse(Pprev, iPprev)))
        ok = false, why = "projection not invertible";
    if (ok)
    {
        M4 VPu = Mul(Mul(Pu, iPc), VP);       // jitter-free current world -> clip
        M4 PVPu = Mul(Mul(Pu, iPprev), PVP);  // jitter-free previous world -> clip
        if (!Inverse(VPu, iVPu))
            ok = false, why = "view-projection not invertible";
        else
        {
            C = Mul(PVPu, iVPu);
            if (!Finite(C) || !Inverse(C, Cinv) || !Inverse(Pu, Pinv))
                ok = false, why = "degenerate";
        }
    }
    double centreMove = 0, rDiff = -1;
    if (ok)
    {
        double q[4] = {0, 0, 0.001, 1}, o[4];
        Apply(C, q, o);
        if (!(o[3] > 0) || fabs(o[0] / o[3]) > 1.5 || fabs(o[1] / o[3]) > 1.5)
            ok = false, why = "implausible motion";
        else
        {
            centreMove = sqrt(o[0] * o[0] + o[1] * o[1]) / o[3] * low[0] * 0.5;
            // compare with the TAA's own reprojection (diagnostic only)
            double vz = (-cz[0] * 0.001 + cz[1]) / (cz[2] * 0.001 - cz[3]);
            double in[4] = {0, 0, vz, 1}, t[4];
            Apply(R, in, t);
            if (fabs(t[3]) > 1e-12)
            {
                double ex = (t[0] / t[3] - o[0] / o[3]) * low[0] * 0.5, ey = (t[1] / t[3] - o[1] / o[3]) * low[1] * 0.5;
                rDiff = sqrt(ex * ex + ey * ey);
            }
        }
    }
    s_lastFitErr = rDiff;
    {
        static uint64_t s_lastLog;
        static LONG s_n, s_ok;
        static double s_maxMove, s_sumDiff;
        static LONG s_nDiff;
        s_n++;
        if (ok)
        {
            s_ok++;
            if (centreMove > s_maxMove)
                s_maxMove = centreMove;
            if (rDiff >= 0)
                s_sumDiff += rDiff, s_nDiff++;
        }
        uint64_t now = NowQpc();
        if (!s_lastLog)
            s_lastLog = now;
        if (QpcToMs(now - s_lastLog) > 5000.0)
        {
            LOGI("FG camera: last 5 s: %ld frames, %ld usable (%s), max camera motion %.1f px, mean |camera - TAA reprojection| %.2f px",
                 s_n, s_ok, ok ? "ok" : why, s_maxMove, s_nDiff ? s_sumDiff / s_nDiff : -1.0);
            s_lastLog = now;
            s_n = s_ok = s_nDiff = 0;
            s_maxMove = s_sumDiff = 0;
        }
    }
    if (!ok)
    {
        AcquireSRWLockExclusive(&s_camLock);
        s_cam.valid = false;
        ReleaseSRWLockExclusive(&s_camLock);
        return;
    }

    CamOut o;
    ToSL(Pu, o.viewToClip);
    ToSL(Pinv, o.clipToView);
    ToSL(C, o.clipToPrev);
    ToSL(Cinv, o.prevToClip);
    for (int i = 0; i < 3; i++)
    {
        o.pos[i] = eye[i];
        o.fwd[i] = dir[i];
        o.up[i] = vtw[i * 4 + 1];     // view->world column 1
        o.right[i] = -vtw[i * 4 + 0]; // so that up = cross(fwd, right) as Streamline expects
    }
    o.qpc = NowQpc();
    o.valid = true;
    AcquireSRWLockExclusive(&s_camLock);
    s_cam = o;
    ReleaseSRWLockExclusive(&s_camLock);
}

// ---------------------------------------------------------------- Streamline hook
// sl::Constants layout (Streamline 2.x, verified with the SDK headers)
enum
{
    kOffVersion = 24,
    kOffViewToClip = 32,
    kOffClipToView = 96,
    kOffClipToPrev = 224,
    kOffPrevToClip = 288,
    kOffPos = 376,
    kOffUp = 388,
    kOffRight = 400,
    kOffFwd = 412,
    kSizeV1 = 452,
    kSizeV2 = 456,
};
typedef int(__cdecl* PFN_slSetConstants)(const void* values, const void* frame, const void* viewport);
static PFN_slSetConstants o_slSetConstants;
static volatile LONG64 s_slCalls, s_slPatched;

static int __cdecl Hk_slSetConstants(const void* values, const void* frame, const void* viewport)
{
    InterlockedIncrement64(&s_slCalls);
    if (!g_cfg.fgCameraData || !values)
        return o_slSetConstants(values, frame, viewport);
    CamOut c;
    AcquireSRWLockShared(&s_camLock);
    c = s_cam;
    ReleaseSRWLockShared(&s_camLock);
    if (!c.valid || QpcToMs(NowQpc() - c.qpc) > 250.0)
        return o_slSetConstants(values, frame, viewport);

    alignas(16) uint8_t buf[512];
    uint64_t ver = *(const uint64_t*)((const uint8_t*)values + kOffVersion);
    size_t sz = ver >= 2 ? kSizeV2 : kSizeV1;
    memcpy(buf, values, sz);
    memcpy(buf + kOffViewToClip, c.viewToClip, 64);
    memcpy(buf + kOffClipToView, c.clipToView, 64);
    memcpy(buf + kOffClipToPrev, c.clipToPrev, 64);
    memcpy(buf + kOffPrevToClip, c.prevToClip, 64);
    memcpy(buf + kOffPos, c.pos, 12);
    memcpy(buf + kOffUp, c.up, 12);
    memcpy(buf + kOffRight, c.right, 12);
    memcpy(buf + kOffFwd, c.fwd, 12);
    LONG64 n = InterlockedIncrement64(&s_slPatched);
    if (n == 1)
    {
        const float* m = c.clipToPrev;
        LOGI("FG camera: real camera matrices now go to frame generation (clipToPrevClip row0 %.4f %.4f %.4f %.4f, pos %.1f "
             "%.1f %.1f)",
             m[0], m[1], m[2], m[3], c.pos[0], c.pos[1], c.pos[2]);
    }
    return o_slSetConstants(buf, frame, viewport);
}

// export-table patch: the entry is a 32-bit RVA, so the jump stub must sit above the module, within 4 GB
static uint8_t* AllocAbove(uint8_t* base, size_t imageSize)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t start = ((uintptr_t)base + imageSize + gran - 1) & ~(gran - 1);
    for (uintptr_t a = start; a < (uintptr_t)base + 0xF0000000ull; a += gran)
    {
        void* m = VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (m)
            return (uint8_t*)m;
    }
    return nullptr;
}

static bool PatchExport(HMODULE mod, const char* name, void* hook, void** orig)
{
    uint8_t* base = (uint8_t*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress)
        return false;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    DWORD* names = (DWORD*)(base + ed->AddressOfNames);
    WORD* ords = (WORD*)(base + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++)
    {
        if (strcmp((const char*)(base + names[i]), name) != 0)
            continue;
        DWORD* slot = &funcs[ords[i]];
        void* cur = base + *slot;
        uint8_t* stub = AllocAbove(base, nt->OptionalHeader.SizeOfImage);
        if (!stub)
            return false;
        // jmp [rip+0] ; dq hook
        stub[0] = 0xFF;
        stub[1] = 0x25;
        *(int32_t*)(stub + 2) = 0;
        *(void**)(stub + 6) = hook;
        FlushInstructionCache(GetCurrentProcess(), stub, 16);
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old))
            return false;
        *orig = cur;
        MemoryBarrier();
        *slot = (DWORD)(stub - base);
        VirtualProtect(slot, 4, old, &old);
        return true;
    }
    return false;
}

static void TryHookInterposer(HMODULE m, const wchar_t* path)
{
    if (o_slSetConstants)
        return;
    void* orig = nullptr;
    if (PatchExport(m, "slSetConstants", (void*)Hk_slSetConstants, &orig))
    {
        o_slSetConstants = (PFN_slSetConstants)orig;
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, a, sizeof(a), nullptr, nullptr);
        LOGI("FG camera: slSetConstants hooked in %s", a);
    }
    else
        LOGE("FG camera: could not hook slSetConstants in the Streamline interposer");
}

// ntdll loader notifications
struct UNICODE_STR
{
    USHORT Length, MaximumLength;
    PWSTR Buffer;
};
struct LDR_NOTIF
{
    ULONG Flags;
    const UNICODE_STR* FullDllName;
    const UNICODE_STR* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
typedef LONG(NTAPI* PFN_LdrRegisterDllNotification)(ULONG, void(CALLBACK*)(ULONG, const LDR_NOTIF*, PVOID), PVOID, PVOID*);
static PVOID s_cookie;

static bool IsInterposer(const UNICODE_STR* n)
{
    static const wchar_t kName[] = L"sl.interposer.dll";
    size_t len = n->Length / 2, kl = 17;
    if (len != kl)
        return false;
    for (size_t i = 0; i < kl; i++)
    {
        wchar_t c = n->Buffer[i];
        if (c >= L'A' && c <= L'Z')
            c = c - L'A' + L'a';
        if (c != kName[i])
            return false;
    }
    return true;
}

static void CALLBACK DllNotify(ULONG reason, const LDR_NOTIF* data, PVOID)
{
    if (reason != 1 || !data || !data->BaseDllName)
        return;
    if (IsInterposer(data->BaseDllName))
        TryHookInterposer((HMODULE)data->DllBase, data->FullDllName ? data->FullDllName->Buffer : L"?");
}

void SlCamInit()
{
    if (!g_cfg.fgCameraData)
    {
        LOGI("FG camera: disabled (FGCameraData=0)");
        return;
    }
    HMODULE already = GetModuleHandleW(L"sl.interposer.dll");
    if (already)
    {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(already, p, MAX_PATH);
        TryHookInterposer(already, p);
    }
    PFN_LdrRegisterDllNotification reg =
        (PFN_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    if (reg)
    {
        LONG st = reg(0, DllNotify, nullptr, &s_cookie);
        LOGI("FG camera: waiting for Streamline (loader notification %s)", st == 0 ? "registered" : "FAILED");
    }
}

void SlCamStats(char* out, size_t n)
{
    _snprintf(out, n, "FG camera: slSetConstants calls %I64d, patched %I64d, last |camera - TAA| %.2f px", s_slCalls, s_slPatched,
              s_lastFitErr);
}
