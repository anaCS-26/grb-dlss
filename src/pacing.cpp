// Frame-pacing diagnostics (PacingLog=1 in [Log]).
//
// Every outermost IDXGISwapChain::Present (per thread) is timestamped together with whether the game
// rendered a new frame (a DLSS evaluation / TAA pass) since the previous Present. With frame generation
// that goes through the app's Present path (e.g. NVIDIA Smooth Motion), the extra Presents are the
// generated frames, so we can see where each generated frame lands between two real ones: 0.5 = evenly
// spaced (smooth), near 0 or 1 = bunched (same fps counter, choppy motion).
#include "common.h"

namespace
{
struct Sample
{
    uint64_t t;      // QPC at Present entry
    uint32_t blockUs; // time spent inside Present
    uint32_t tid;
    uint8_t real;     // a new game frame was rendered since the previous Present
};

const int kMax = 8192;
Sample s_buf[kMax];
volatile LONG s_n;
volatile LONG s_newFrame; // set by an evaluation, consumed by the next Present
SRWLOCK s_lock = SRWLOCK_INIT;
uint64_t s_windowStart;

// per-thread Present nesting (wrappers call into the real swap chain on the same thread)
struct Nest
{
    DWORD tid;
    LONG depth;
};
Nest s_nest[32];
SRWLOCK s_nestLock = SRWLOCK_INIT;

LONG* DepthFor(DWORD tid)
{
    for (int i = 0; i < 32; i++)
        if (s_nest[i].tid == tid)
            return &s_nest[i].depth;
    for (int i = 0; i < 32; i++)
        if (s_nest[i].tid == 0)
        {
            s_nest[i].tid = tid;
            s_nest[i].depth = 0;
            return &s_nest[i].depth;
        }
    return nullptr;
}

void SortD(double* a, int n)
{
    for (int i = 1; i < n; i++) // insertion sort, n is small (<= a few thousand, once per window)
    {
        double v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v)
        {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = v;
    }
}

double s_tmpA[kMax], s_tmpB[kMax];

void Report(const Sample* s, int n)
{
    if (n < 10)
        return;
    double ms = QpcToMs(s[n - 1].t - s[0].t);
    // present-to-present intervals
    int ni = 0;
    for (int i = 1; i < n; i++)
        s_tmpA[ni++] = QpcToMs(s[i].t - s[i - 1].t);
    SortD(s_tmpA, ni);
    double pMed = s_tmpA[ni / 2], p5 = s_tmpA[ni * 5 / 100], p95 = s_tmpA[ni * 95 / 100];
    int under6 = 0, over25 = 0;
    for (int i = 0; i < ni; i++)
    {
        under6 += s_tmpA[i] < 6.0;
        over25 += s_tmpA[i] > 25.0;
    }
    // real-to-real intervals and where the in-between Presents land
    int nr = 0, nsplit = 0, reals = 0;
    int hist[5] = {}; // split position buckets: <0.2, <0.4, 0.4-0.6, >0.6, >0.8
    double prevReal = -1;
    int lastRealIdx = -1;
    for (int i = 0; i < n; i++)
    {
        if (!s[i].real)
            continue;
        reals++;
        if (lastRealIdx >= 0)
        {
            double dt = QpcToMs(s[i].t - s[lastRealIdx].t);
            s_tmpB[nr++] = dt;
            for (int k = lastRealIdx + 1; k < i; k++)
            {
                double f = QpcToMs(s[k].t - s[lastRealIdx].t) / (dt > 0 ? dt : 1);
                int b = f < 0.2 ? 0 : f < 0.4 ? 1 : f <= 0.6 ? 2 : f <= 0.8 ? 3 : 4;
                hist[b]++;
                nsplit++;
            }
        }
        lastRealIdx = i;
        (void)prevReal;
    }
    double rMed = 0, r95 = 0, rMean = 0, rSd = 0;
    if (nr > 4)
    {
        for (int i = 0; i < nr; i++)
            rMean += s_tmpB[i];
        rMean /= nr;
        for (int i = 0; i < nr; i++)
            rSd += (s_tmpB[i] - rMean) * (s_tmpB[i] - rMean);
        rSd = __builtin_sqrt(rSd / nr);
        SortD(s_tmpB, nr);
        rMed = s_tmpB[nr / 2];
        r95 = s_tmpB[nr * 95 / 100];
    }
    // threads: real presents vs the others
    uint32_t tReal = 0, tOther = 0;
    int otherThreads = 0;
    for (int i = 0; i < n; i++)
    {
        if (s[i].real && !tReal)
            tReal = s[i].tid;
        if (!s[i].real && s[i].tid != tReal && tReal)
        {
            if (!tOther)
                tOther = s[i].tid;
            otherThreads++;
        }
    }
    double blockSum = 0, blockMax = 0;
    for (int i = 0; i < n; i++)
    {
        double b = s[i].blockUs / 1000.0;
        blockSum += b;
        if (b > blockMax)
            blockMax = b;
    }
    LOGI("Pacing %.1fs: presents %.1f/s (interval med %.1f p5 %.1f p95 %.1f ms, <6ms %d%% >25ms %d%%) | game frames %.1f/s "
         "(med %.1f mean %.1f sd %.1f p95 %.1f ms) | in-between presents %d at 0-.2:%d .2-.4:%d .4-.6:%d .6-.8:%d .8-1:%d | "
         "present block avg %.2f max %.1f ms | game thread %u, other-thread presents %d (thread %u)",
         ms / 1000.0, (n - 1) * 1000.0 / ms, pMed, p5, p95, under6 * 100 / ni, over25 * 100 / ni, reals * 1000.0 / ms, rMed,
         rMean, rSd, r95, nsplit, hist[0], hist[1], hist[2], hist[3], hist[4], blockSum / n, blockMax, tReal, otherThreads,
         tOther);
}
} // namespace

void PacingOnEval()
{
    InterlockedExchange(&s_newFrame, 1);
}

// returns true when this call is the outermost Present on its thread (caller must call PacingPresentEnd)
bool PacingPresentBegin()
{
    DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&s_nestLock);
    LONG* d = DepthFor(tid);
    bool outer = d && (*d)++ == 0;
    ReleaseSRWLockExclusive(&s_nestLock);
    return outer;
}

void PacingPresentEnd(bool outer, uint64_t t0, uint64_t t1)
{
    DWORD tid = GetCurrentThreadId();
    AcquireSRWLockExclusive(&s_nestLock);
    LONG* d = DepthFor(tid);
    if (d && *d > 0)
        (*d)--;
    ReleaseSRWLockExclusive(&s_nestLock);
    if (!outer || !g_cfg.pacingLog)
        return;

    AcquireSRWLockExclusive(&s_lock);
    if (!s_windowStart)
        s_windowStart = t0;
    if (s_n < kMax)
    {
        Sample& s = s_buf[s_n++];
        s.t = t0;
        double us = QpcToMs(t1 - t0) * 1000.0;
        s.blockUs = us > 4e9 ? 4000000000u : (uint32_t)us;
        s.tid = tid;
        s.real = (uint8_t)InterlockedExchange(&s_newFrame, 0);
    }
    if (QpcToMs(t0 - s_windowStart) >= 10000.0 || s_n >= kMax)
    {
        Report(s_buf, s_n);
        s_n = 0;
        s_windowStart = t0;
    }
    ReleaseSRWLockExclusive(&s_lock);
}
