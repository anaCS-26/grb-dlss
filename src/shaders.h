#pragma once
// HLSL sources compiled at runtime with d3dcompiler_47.
// Constant buffer layouts mirror the game's own (from the shader reflection data in
// shadercontainer_engine_win64_f.dll): cb_g_TemporalAA_Constants (b5, 192 bytes),
// cb_g_Pass (b2, clipZToViewZ @ byte 320, m_ProjMatrix @ 640), cb_g_Frame (b1, m_HdrParams @ 2112).

static const char kCommonHlsl[] = R"HLSL(
cbuffer TAACB : register(b5)
{
    float4 Reproj0;
    float4 Reproj1;
    float4 Reproj2;
    float4 Reproj3;
    uint4  IsolationVars;
    float2 FullResSize;
    float2 FullResSizeInv;
    float2 LowResSize;
    float2 LowResSizeInv;
    float2 SubPixelJitter;
    float  RejectionAvoidanceMax;
    float  RejectionAvoidanceMaxInv;
    float3 NoiseOffsetScale;
    float  MinLiveColorNear;
    float4 MinLiveColorMisc;
    float4 MaxViewDepthParams;
    uint   HdrEnabled;
    float  HdrNormalizeSrgb;
    float  HdrDenormalizeSrgb;
    uint   NoMotionVector;
};
)HLSL";

// Prepares DLSS inputs at render resolution:
//  - colour copy (HDR mode: converted to linear, as the game does after its TAA)
//  - full motion vectors: the game's object-motion buffer only holds dynamic objects, static pixels
//    are reprojected from depth with the TAA's own ReprojectionMatrix (identical maths to the game).
//  - depth copy (R32F)
//  - optional responsive/bias mask
static const char kPrepCS[] = R"HLSL(
cbuffer PassCB : register(b2)
{
    float4 PassPad0[20];
    float4 ClipZToViewZ;     // reverseProjParams.clipZToViewZ
    float4 PassPad1[19];
    float4 Proj0;            // m_ProjMatrix rows
    float4 Proj1;
    float4 Proj2;
    float4 Proj3;
};
cbuffer ModCB : register(b0)
{
    uint  ModFlags;          // bit0: HDR linearise, bit1: write bias mask
    float3 ModPad;
};

Texture2D<float4> tColor      : register(t0);
Texture2D<float4> tMotion     : register(t1);
Texture2D<float>  tDepth      : register(t2);
Texture2D<float4> tResponsive : register(t3);
Texture2D<float4> tPrevColor  : register(t4);

RWTexture2D<float4> oColor : register(u0);
RWTexture2D<float2> oMV    : register(u1);
RWTexture2D<float>  oDepth : register(u2);
RWTexture2D<float>  oBias  : register(u3);
RWBuffer<float>     oInfo  : register(u4);

float ViewZ(float d)
{
    return (-ClipZToViewZ.x * d + ClipZToViewZ.y) / (ClipZToViewZ.z * d - ClipZToViewZ.w);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    uint2 size = (uint2)LowResSize;
    if (id.x == 0 && id.y == 0)
    {
        // camera info for the CPU (read back asynchronously): reversed-Z, near, far, |P11|, |P00|
        float z0 = ViewZ(0.0);
        float z1 = ViewZ(1.0);
        bool rev = abs(z0) > abs(z1);
        oInfo[0] = rev ? 1.0 : 0.0;
        oInfo[1] = rev ? z1 : z0;
        oInfo[2] = rev ? z0 : z1;
        oInfo[3] = Proj1.y;
        oInfo[4] = Proj0.x;
        oInfo[5] = 1.0;
    }
    if (id.x == 1 && id.y == 0 && NoMotionVector == 0)
    {
        // diagnostics: mean motion (render pixels) of object-motion pixels (game vs depth reprojection) and static pixels
        float on = 0, sn = 0, dsum = 0;
        float2 og = 0, orp = 0, st = 0;
        float nn = 0, eG = 0, eR = 0, eZ = 0, nG = 0, nR = 0, fn = 0, fR = 0, fZ = 0;
        for (uint gy = 0; gy < 27; gy++)
            for (uint gx = 0; gx < 48; gx++)
            {
                uint2 q = uint2((gx + 0.5) * size.x / 48.0, (gy + 0.5) * size.y / 27.0);
                float dq = tDepth.Load(int3(q, 0));
                float2 ndc = (float2(q) + 0.5) * LowResSizeInv * float2(2.0, -2.0) + float2(-1.0, 1.0);
                float4 pp = float4(ndc, ViewZ(dq), 1.0);
                float  w = dot(pp, Reproj3);
                float2 prev = float2(dot(pp, Reproj0), dot(pp, Reproj1)) / w;
                float2 rv = (prev - ndc) * float2(0.5, -0.5) * LowResSize;
                if (any(!(abs(rv) < 4096.0))) rv = 0;
                float2 m = tMotion.Load(int3(q, 0)).xy;
                {
                    // reprojection error: does the previous frame's colour at q + mv match the current colour?
                    float3 cc = tColor.Load(int3(q, 0)).rgb;
                    float2 gv2 = dot(m, m) != 0.0 ? m * 0.5 * LowResSize : rv;
                    int2 pg = clamp(int2(float2(q) + 0.5 + gv2), int2(0, 0), int2(size) - 1);
                    int2 pr = clamp(int2(float2(q) + 0.5 + rv), int2(0, 0), int2(size) - 1);
                    float3 lw = float3(0.299, 0.587, 0.114);
                    float e_g = abs(dot(cc - tPrevColor.Load(int3(pg, 0)).rgb, lw));
                    float e_r = abs(dot(cc - tPrevColor.Load(int3(pr, 0)).rgb, lw));
                    float e_z = abs(dot(cc - tPrevColor.Load(int3(q, 0)).rgb, lw));
                    float2 uvq = (float2(q) + 0.5) * LowResSizeInv;
                    bool inBox = uvq.x > 0.12 && uvq.x < 0.45 && uvq.y > 0.22 && uvq.y < 0.95 && dot(m, m) != 0.0;
                    if (inBox)
                    {
                        nn += 1; eG += e_g; eR += e_r; eZ += e_z; nG += length(gv2); nR += length(rv);
                    }
                    else
                    {
                        fn += 1; fR += e_r; fZ += e_z;
                    }
                }
                if (dot(m, m) != 0.0)
                {
                    float2 gv = m * 0.5 * LowResSize;
                    on += 1; og += gv; orp += rv; dsum += length(gv - rv);
                }
                else
                {
                    sn += 1; st += rv;
                }
            }
        oInfo[8] = on;
        oInfo[9] = on > 0 ? og.x / on : 0;
        oInfo[10] = on > 0 ? og.y / on : 0;
        oInfo[11] = on > 0 ? orp.x / on : 0;
        oInfo[12] = on > 0 ? orp.y / on : 0;
        oInfo[13] = on > 0 ? dsum / on : 0;
        oInfo[14] = sn;
        oInfo[15] = sn > 0 ? st.x / sn : 0;
        oInfo[16] = sn > 0 ? st.y / sn : 0;
        oInfo[19] = nn;
        oInfo[20] = nn > 0 ? eG / nn : 0;
        oInfo[21] = nn > 0 ? eR / nn : 0;
        oInfo[22] = nn > 0 ? eZ / nn : 0;
        oInfo[23] = nn > 0 ? nG / nn : 0;
        oInfo[24] = nn > 0 ? nR / nn : 0;
        oInfo[25] = fn;
        oInfo[26] = fn > 0 ? fR / fn : 0;
        oInfo[27] = fn > 0 ? fZ / fn : 0;
        oInfo[17] = SubPixelJitter.x;
        oInfo[18] = SubPixelJitter.y;
    }
    if (id.x >= size.x || id.y >= size.y)
        return;

    float4 c = tColor.Load(int3(id, 0));
    if (ModFlags & 1u)
    {
        c.rgb = pow(max(c.rgb * HdrDenormalizeSrgb, 0.0), 2.2);
    }
    oColor[id] = c;

    float d = tDepth.Load(int3(id, 0));
    oDepth[id] = d;

    float2 vel = float2(0.0, 0.0);
    if (NoMotionVector == 0)
    {
        float2 m = tMotion.Load(int3(id, 0)).xy;
        if (dot(m, m) != 0.0)
        {
            vel = m * 0.5;
        }
        else
        {
            float2 ndc = (float2(id) + 0.5) * LowResSizeInv * float2(2.0, -2.0) + float2(-1.0, 1.0);
            float4 p = float4(ndc, ViewZ(d), 1.0);
            float  w = dot(p, Reproj3);
            float2 prev = float2(dot(p, Reproj0), dot(p, Reproj1)) / w;
            vel = (prev - ndc) * float2(0.5, -0.5);
            if (any(!(abs(vel) < 4.0)))  // catches NaN/Inf (e.g. infinite far plane)
                vel = float2(0.0, 0.0);
        }
    }
    oMV[id] = vel;   // UV units, current -> previous; scaled to pixels via DLSS MV scale

    if (ModFlags & 4u)
    {
        // debug: how badly does the previous frame, moved by these motion vectors, match this frame?
        int2 pp = clamp(int2(float2(id) + 0.5 + vel * LowResSize), int2(0, 0), int2(size) - 1);
        float e = abs(dot(c.rgb - tPrevColor.Load(int3(pp, 0)).rgb, float3(0.299, 0.587, 0.114)));
        oBias[id] = saturate(e * 4.0);
    }
    else if (ModFlags & 2u)
        oBias[id] = saturate(tResponsive.Load(int3(id, 0)).x);
}
)HLSL";

// Replaces the game's TAA pixel shader for the TAA draw itself, so the game's own fullscreen pass
// writes the DLSS result to the TAA output with all of the game's state intact.
static const char kResolvePS[] = R"HLSL(
cbuffer FrameCB : register(b1)
{
    float4 FramePad[132];
    uint   UseGamma22;
    float  MaxDisplayLuminance;
    float  PeakBrightness;
    float  PaperWhite;
};
cbuffer ModPS : register(b0)
{
    uint  DebugView;
    float3 ModPad;
};
Texture2D<float4> tDlss      : register(t0);
Texture2D<float4> tDebugMV   : register(t1);
Texture2D<float>  tDebugZ    : register(t2);
Texture2D<float4> tGameColor : register(t13);
Texture2D<float4> tGameMotion : register(t14);
Texture2D<float>  tGameDepth  : register(t15);

struct PSOut
{
    float4 c0 : SV_Target0;
    float3 c1 : SV_Target1;
    float  c2 : SV_Target2;
};

PSOut main(float4 pos : SV_Position, float2 uv : TEXCOORD0)
{
    int2 p = int2(pos.xy);
    float3 c = tDlss.Load(int3(p, 0)).rgb;

    int2 lp = int2(pos.xy * FullResSizeInv * LowResSize);
    lp = clamp(lp, int2(0, 0), int2(LowResSize) - 1);
    float a = tGameColor.Load(int3(lp, 0)).a;

    if (DebugView == 1u)
    {
        float2 mv = tDebugMV.Load(int3(lp, 0)).xy * LowResSize;
        c = float3(saturate(abs(mv) * 0.1), 0.0);
    }
    else if (DebugView == 3u)
    {
        // red = pixel carries the game's own (object) motion, grey = reprojected from depth
        float2 gm = tGameMotion.Load(int3(lp, 0)).xy;
        float l = dot(c, float3(0.299, 0.587, 0.114)) * 0.6;
        c = (dot(gm, gm) != 0.0) ? float3(1.0, 0.15, 0.1) * (0.4 + l) : float3(l, l, l);
    }
    else if (DebugView == 4u)
    {
        // signed motion in render pixels: x -> red/cyan, y -> green/magenta, brightness = speed
        float2 mv = tDebugMV.Load(int3(lp, 0)).xy * LowResSize;
        c = float3(saturate(mv.x * 0.05), saturate(mv.y * 0.05), saturate(-mv.x * 0.05)) + saturate(-mv.y * 0.05) * float3(1, 0, 1) * 0.5;
    }
    else if (DebugView == 5u)
    {
        // reprojection error (bright = the motion vectors don't explain this pixel), over a dim image
        float e = tDebugZ.Load(int3(lp, 0));
        float l = dot(c, float3(0.299, 0.587, 0.114)) * 0.35;
        c = float3(l, l, l) + float3(e, e * 0.6, 0.0);
    }
    else if (DebugView == 2u)
    {
        float z = tDebugZ.Load(int3(lp, 0));
        c = frac(float3(z, z * 16.0, z * 256.0));
    }

    PSOut o;
    o.c2 = 1.0;
    if (HdrEnabled != 0)
    {
        // DLSS ran on linear values; continue exactly like the game's TAA output path.
        float3 lin = max(c, 0.0);
        o.c1 = saturate(pow(lin, 1.0 / 2.2) * HdrNormalizeSrgb);
        float3 outc;
        if (UseGamma22 == 0)
        {
            float3 r;
            r.x = dot(float3(0.627403914928436, 0.329282641410828, 0.0433130674064159), lin);
            r.y = dot(float3(0.0690972879528999, 0.919540405273438, 0.0113623151555657), lin);
            r.z = dot(float3(0.0163914952427149, 0.0880133062601089, 0.895595014095306), lin);
            r = r * PaperWhite * 0.0001;
            float3 pp = pow(abs(r), 0.1593017578125);
            outc = pow((pp * 18.8515625 + 0.8359375) / (pp * 18.6875 + 1.0), 78.84375);
        }
        else
        {
            outc = pow(max(lin * (PaperWhite / MaxDisplayLuminance), 0.0), 0.454545454545);
        }
        o.c0 = float4(outc, a);
    }
    else
    {
        o.c1 = saturate(c);
        o.c0 = float4(c, a);
    }
    return o;
}
)HLSL";

// Full-resolution, depth-dilated motion vectors (used instead of the render-resolution ones when
// DilatedMotionVectors=1): every output pixel takes the motion of the closest-to-camera sample in the 3x3
// render-resolution neighbourhood, so foreground motion (the player character) covers its own silhouette
// edge. This is the "dilated MV" format DLSS accepts for high-res MVs and that frame generation prefers.
static const char kDilateCS[] = R"HLSL(
cbuffer ModCB : register(b0)
{
    uint  ModFlags;          // bit0: reversed depth
    float3 ModPad;
};
Texture2D<float2> tMVLow    : register(t0);
Texture2D<float>  tDepthLow : register(t1);
RWTexture2D<float2> oMVHi   : register(u0);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    uint2 full = (uint2)FullResSize;
    if (id.x >= full.x || id.y >= full.y)
        return;
    int2 lowMax = int2(LowResSize) - 1;
    float2 l = (float2(id) + 0.5) * LowResSize * FullResSizeInv;
    int2 c = clamp(int2(l), int2(0, 0), lowMax);
    bool rev = (ModFlags & 1u) != 0;
    int2 best = c;
    float bestD = tDepthLow.Load(int3(c, 0));
    [unroll] for (int dy = -1; dy <= 1; dy++)
    [unroll] for (int dx = -1; dx <= 1; dx++)
    {
        int2 q = clamp(c + int2(dx, dy), int2(0, 0), lowMax);
        float d = tDepthLow.Load(int3(q, 0));
        if (rev ? (d > bestD) : (d < bestD))
        {
            bestD = d;
            best = q;
        }
    }
    oMVHi[id] = tMVLow.Load(int3(best, 0));
}
)HLSL";
