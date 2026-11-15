#include "NullTest.h"

#include <cmath>
#include <complex>
#include <vector>

namespace lathe {

namespace {

using cplx = std::complex<double>;

void fft(std::vector<cplx>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * juce::MathConstants<double>::pi / double(len) * (inverse ? 1 : -1);
        cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                cplx u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= double(n);
}

// Mono (L+R) samples [start, start+n) of a file; zeros outside the file.
std::vector<double> readMono(juce::AudioFormatReader& r, juce::int64 start, int n) {
    std::vector<double> out(size_t(n), 0.0);
    juce::AudioBuffer<float> buf(2, n);
    buf.clear();
    juce::int64 a = juce::jmax<juce::int64>(0, start), b = juce::jmin<juce::int64>(r.lengthInSamples, start + n);
    if (b > a) {
        r.read(&buf, int(a - start), int(b - a), a, true, true);
        if (r.numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, n);
    }
    for (int i = 0; i < n; ++i) out[size_t(i)] = double(buf.getSample(0, i)) + double(buf.getSample(1, i));
    return out;
}

// Stereo samples [start, start+n) of a file (mono copied to both); zeros outside the file.
juce::AudioBuffer<float> readStereo(juce::AudioFormatReader& r, juce::int64 start, int n) {
    juce::AudioBuffer<float> buf(2, juce::jmax(1, n));
    buf.clear();
    if (n <= 0) return buf;
    juce::int64 a = juce::jmax<juce::int64>(0, start), b = juce::jmin<juce::int64>(r.lengthInSamples, start + n);
    if (b > a) {
        r.read(&buf, int(a - start), int(b - a), a, true, true);
        if (r.numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, n);
    }
    return buf;
}

juce::String db(double lin) {
    if (lin <= 0) return "-inf dBFS";
    return juce::String(20.0 * std::log10(lin), 1) + " dBFS";
}

juce::String clock(double sec) {
    const long long ms = std::llround(juce::jmax(0.0, sec) * 1000.0);
    return juce::String::formatted("%02d:%02d.%03d", int(ms / 60000), int((ms / 1000) % 60), int(ms % 1000));
}

// ---- Alignment

// Mean of |L| + |R| over each block of D samples of the whole file.
bool envelope(juce::AudioFormatReader& r, int D, std::vector<double>& env, const std::function<bool(double)>& tick) {
    const juce::int64 len = r.lengthInSamples;
    env.assign(size_t(juce::jmax<juce::int64>(0, (len + D - 1) / D)), 0.0);
    const int chunk = juce::jmax(65536, D);   // a multiple of D (D is a power of two)
    juce::AudioBuffer<float> buf(2, chunk);
    for (juce::int64 p = 0; p < len; p += chunk) {
        const int n = int(juce::jmin<juce::int64>(chunk, len - p));
        r.read(&buf, 0, n, p, true, true);
        if (r.numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, n);
        const float* L = buf.getReadPointer(0);
        const float* R = buf.getReadPointer(1);
        for (int i = 0; i < n; i += D) {
            const int m = juce::jmin(D, n - i);
            double s = 0;
            for (int j = 0; j < m; ++j) s += std::abs(double(L[i + j])) + std::abs(double(R[i + j]));
            env[size_t((p + i) / D)] = s / double(m);
        }
        if (tick && !tick(double(p + n) / double(len))) return false;
    }
    return true;
}

struct Candidate {
    juce::int64 centre = 0;   // coarse offset (B lags A), samples
    juce::int64 startA = 0;   // where in A to take the fine window
    int maxLag = 0;           // fine search range either side of centre
};

// Coarse offsets from the two whole-file envelopes (best first), each with a window that is
// loud in both files once that offset is applied.
std::vector<Candidate> coarseCandidates(const std::vector<double>& ea, const std::vector<double>& eb, int D, int N) {
    std::vector<Candidate> out;
    const juce::int64 nA = juce::int64(ea.size()), nB = juce::int64(eb.size());
    const juce::int64 minOverlap = 16;
    if (nA < minOverlap || nB < minOverlap) return out;
    double mA = 0, mB = 0;
    for (double v : ea) mA += v;
    for (double v : eb) mB += v;
    mA /= double(nA);
    mB /= double(nB);

    // One complex FFT carries both real sequences: A in the real part, B in the imaginary part.
    size_t M = 1;
    while (M < size_t(nA + nB)) M <<= 1;
    std::vector<cplx> z(M);
    std::vector<double> pa(size_t(nA) + 1, 0.0), pb(size_t(nB) + 1, 0.0);   // prefix sums of squares
    for (juce::int64 i = 0; i < nA; ++i) {
        const double x = ea[size_t(i)] - mA;
        z[size_t(i)].real(x);
        pa[size_t(i) + 1] = pa[size_t(i)] + x * x;
    }
    for (juce::int64 i = 0; i < nB; ++i) {
        const double y = eb[size_t(i)] - mB;
        z[size_t(i)].imag(y);
        pb[size_t(i) + 1] = pb[size_t(i)] + y * y;
    }
    fft(z, false);
    for (size_t k = 0; k <= M / 2; ++k) {
        const size_t k2 = (M - k) & (M - 1);
        const cplx zk = z[k], zc = std::conj(z[k2]);
        const cplx fa = (zk + zc) * 0.5, fb = (zk - zc) * cplx(0, -0.5);
        const cplx pk = std::conj(fa) * fb;   // spectrum of sum_i a[i] b[i + lag]
        z[k] = pk;
        z[k2] = std::conj(pk);
    }
    fft(z, true);

    // Normalised correlation weighted by sqrt(overlap): about 1 per sqrt(block) for chance
    // alignments, sqrt(overlap) for the real one.
    auto score = [&](juce::int64 L) {
        const juce::int64 i0 = juce::jmax<juce::int64>(0, -L), i1 = juce::jmin(nA, nB - L), ov = i1 - i0;
        if (ov < minOverlap) return 0.0;
        const double EA = pa[size_t(i1)] - pa[size_t(i0)], EB = pb[size_t(i1 + L)] - pb[size_t(i0 + L)];
        if (EA <= 0 || EB <= 0) return 0.0;
        const juce::int64 m = juce::int64(M);
        const double c = z[size_t(((L % m) + m) % m)].real();
        return c / std::sqrt(EA * EB) * std::sqrt(double(ov));
    };
    std::vector<juce::int64> picks;
    for (int round = 0; round < 3; ++round) {
        double bestS = 0;
        juce::int64 bestL = 0;
        bool any = false;
        for (juce::int64 L = -(nA - 1); L <= nB - 1; ++L) {
            bool taken = false;
            for (auto q : picks)
                if (std::abs(L - q) <= 4) taken = true;
            if (taken) continue;
            const double s = score(L);
            if (s > bestS) { bestS = s; bestL = L; any = true; }
        }
        if (!any) break;
        picks.push_back(bestL);
    }

    const juce::int64 w = juce::jmax<juce::int64>(1, (N + D - 1) / D);
    for (auto L : picks) {
        const juce::int64 i0 = juce::jmax<juce::int64>(0, -L), i1 = juce::jmin(nA, nB - L);
        juce::int64 start = i0;
        if (i1 - i0 > w) {
            // The first window that is at least half as loud (in both files) as the loudest one.
            auto both = [&](juce::int64 i) { return juce::jmin(ea[size_t(i)], eb[size_t(i + L)]); };
            double s = 0, best = 0;
            for (juce::int64 i = i0; i < i0 + w; ++i) s += both(i);
            best = s;
            for (juce::int64 i = i0 + 1; i + w <= i1; ++i) {
                s += both(i + w - 1) - both(i - 1);
                best = juce::jmax(best, s);
            }
            s = 0;
            for (juce::int64 i = i0; i < i0 + w; ++i) s += both(i);
            for (juce::int64 i = i0; i + w <= i1; ++i) {
                if (i > i0) s += both(i + w - 1) - both(i - 1);
                if (s >= 0.5 * best) { start = i; break; }
            }
        }
        Candidate c;
        c.centre = L * D;
        c.startA = start * D;
        c.maxLag = juce::jmax(16384, 4 * D);   // wide: steady tones give the envelope near-miss peaks
        out.push_back(c);
    }
    return out;
}

struct Fine {
    juce::int64 offset = 0;
    bool inverted = false;
    double score = 0;   // |normalised correlation| at the peak, 0..1
};

// Sample-accurate cross-correlation of N samples of A at startA against B around centre +/- maxLag.
Fine fineAlign(juce::AudioFormatReader& A, juce::AudioFormatReader& B, juce::int64 startA, juce::int64 centre, int maxLag, int N) {
    auto a = readMono(A, startA, N);
    auto b = readMono(B, startA + centre - maxLag, N + 2 * maxLag);
    size_t M = 1;
    while (M < a.size() + b.size()) M <<= 1;
    std::vector<cplx> fa2(M), fb2(M);
    for (size_t i = 0; i < a.size(); ++i) fa2[i] = a[i];
    for (size_t i = 0; i < b.size(); ++i) fb2[i] = b[i];
    fft(fa2, false);
    fft(fb2, false);
    for (size_t i = 0; i < M; ++i) fa2[i] = std::conj(fa2[i]) * fb2[i];
    fft(fa2, true);
    int best = maxLag;
    double bestV = 0;
    for (int k = 0; k <= 2 * maxLag; ++k) {
        double v = fa2[size_t(k)].real();
        if (std::abs(v) > std::abs(bestV)) { bestV = v; best = k; }
    }
    Fine f;
    f.offset = centre + best - maxLag;
    f.inverted = bestV < 0;
    double eA = 0, eB = 0;
    for (double v : a) eA += v * v;
    for (int i = 0; i < N; ++i) eB += b[size_t(best + i)] * b[size_t(best + i)];
    f.score = eA > 0 && eB > 0 ? std::abs(bestV) / std::sqrt(eA * eB) : 0.0;
    return f;
}

// ---- Difference events

struct Event {
    juce::int64 start = 0, last = 0;   // A positions of the first and last differing samples
    double peak = 0;
    juce::int64 off = 0;               // offset in effect: B position = A position + off
    int dropK = 0;                     // != 0: the files re-aligned here by this shift
    int retries = 0;
};

// After a difference at A position `pos` (B at pos + off), looks for a constant shift k != 0 that
// makes the files agree again exactly: A[n] == B[n + off + k] from just after the difference on.
// k > 0: B has k extra samples; k < 0: B is missing -k samples (A's [pos, pos - k) have no
// counterpart). Returns 0 when they agree again with no shift, the audio is too quiet to tell, or
// no shift works (then `failed` is set).
int findShift(juce::AudioFormatReader& A, juce::AudioFormatReader& B, juce::int64 pos, juce::int64 off, double pol, bool& failed) {
    const int T = 64, W = 2000, K = 8192;
    const double tol = 1e-6;
    const juce::int64 a0 = pos + T;
    auto a = readStereo(A, a0, K + W);              // a[j] = A[a0 + j]
    auto b = readStereo(B, a0 + off - K, 2 * K + W);  // b[j] = B[a0 + off - K + j]
    auto agrees = [&](int k) {
        const int ai = juce::jmax(0, -k), bi = ai + k + K;
        const juce::int64 aPos = a0 + ai, bPos = a0 + off + ai + k;
        if (aPos + W > A.lengthInSamples || bPos < 0 || bPos + W > B.lengthInSamples) return false;
        const float* xl = a.getReadPointer(0, ai);
        const float* xr = a.getReadPointer(1, ai);
        const float* yl = b.getReadPointer(0, bi);
        const float* yr = b.getReadPointer(1, bi);
        for (int j = 0; j < W; ++j)
            if (std::abs(double(xl[j]) - pol * double(yl[j])) > tol || std::abs(double(xr[j]) - pol * double(yr[j])) > tol) return false;
        return true;
    };
    if (agrees(0)) return 0;
    if (a.getMagnitude(0, W) < 0.01f) return 0;   // too quiet to tell one shift from another
    for (int m = 1; m <= K; ++m) {
        if (agrees(-m)) return -m;
        if (agrees(m)) return m;
    }
    failed = true;
    return 0;
}

bool closeTo(const juce::AudioBuffer<float>& x, int xi, const juce::AudioBuffer<float>& y, int yi, int n, double tol) {
    for (int ch = 0; ch < 2; ++ch) {
        const float* p = x.getReadPointer(ch, xi);
        const float* q = y.getReadPointer(ch, yi);
        for (int j = 0; j < n; ++j)
            if (std::abs(double(p[j]) - double(q[j])) > tol) return false;
    }
    return true;
}

// If the samples [xs, xs + len) of a file repeat that same file's audio from L samples earlier
// (and the audio just before them doesn't, so it isn't simply periodic material), returns L.
// Tries powers of two from 256 to 2^20 (events of 2+ samples), then every lag up to 2^17 (8+).
juce::int64 repeatLag(juce::AudioFormatReader& r, juce::int64 xs, int len) {
    const double tol = 1e-7;
    const int pre = 64;
    if (len <= 0 || xs <= 0) return 0;
    auto cur = readStereo(r, xs - pre, pre + len);   // cur[pre + j] = X[xs + j]
    if (cur.getMagnitude(pre, len) < 1e-4f) return 0;
    const bool loudBefore = cur.getMagnitude(0, pre) > 1e-4f;
    // h[base + i] = X[xs - L - pre + i]
    auto matchAt = [&](const juce::AudioBuffer<float>& h, int base) {
        if (!closeTo(h, base + pre, cur, pre, len, tol)) return false;
        return !(loudBefore && closeTo(h, base, cur, 0, pre, tol));
    };
    const int maxL = int(juce::jmin<juce::int64>(1 << 17, xs));
    auto hist = readStereo(r, xs - maxL - pre, maxL + pre + len);   // hist[i] = X[xs - maxL - pre + i]
    // A sample or two of quiet 16-bit audio can match somewhere by chance: insist on a few.
    if (len < 2) return 0;
    for (juce::int64 L = 256; L <= (1 << 20) && L <= xs; L <<= 1) {
        if (L <= maxL) {
            if (matchAt(hist, int(maxL - L))) return L;
        } else {
            auto h = readStereo(r, xs - L - pre, pre + len);
            if (matchAt(h, 0)) return L;
        }
    }
    if (len < 8) return 0;
    for (int L = 1; L <= maxL; ++L) {
        if (L >= 256 && juce::isPowerOfTwo(L)) continue;
        if (matchAt(hist, maxL - L)) return L;
    }
    return 0;
}

// Which file has the click: the one whose second difference spikes far above its neighbourhood.
juce::String clickFile(juce::AudioFormatReader& A, juce::AudioFormatReader& B, juce::int64 start, juce::int64 off, int len) {
    auto ratio = [len](juce::AudioFormatReader& X, juce::int64 xs) {
        const int W = 256;
        auto buf = readStereo(X, xs - W - 1, len + 2 * W + 2);   // buf[i] = X[xs - W - 1 + i]
        const int n = buf.getNumSamples();
        const float* l = buf.getReadPointer(0);
        const float* r = buf.getReadPointer(1);
        double inMax = 0, outSum = 0;
        int outN = 0;
        for (int i = 1; i + 1 < n; ++i) {
            const double d2 = juce::jmax(std::abs(double(l[i + 1]) - 2.0 * double(l[i]) + double(l[i - 1])),
                                         std::abs(double(r[i + 1]) - 2.0 * double(r[i]) + double(r[i - 1])));
            if (i >= W && i <= W + len + 1) inMax = juce::jmax(inMax, d2);
            else { outSum += d2 * d2; ++outN; }
        }
        return inMax / (std::sqrt(outSum / double(juce::jmax(1, outN))) + 1e-9);
    };
    const double rA = ratio(A, start), rB = ratio(B, start + off);
    if (rA > 4 && rA > 3 * rB) return "A";
    if (rB > 4 && rB > 3 * rA) return "B";
    return "?";
}

}  // namespace

// The offset at which B best matches A: a search around "started together", then coarse offsets
// from the whole of both files' decimated envelopes, each refined to the sample. `ok` false = cancelled.
static Fine findAlignment(juce::AudioFormatReader& A_, juce::AudioFormatReader& B_, const std::function<bool(double)>& stage, bool& ok) {
    auto* A = &A_;
    auto* B = &B_;
    struct { double rateA; juce::int64 lengthA, lengthB; } r{A->sampleRate, A->lengthInSamples, B->lengthInSamples};
    ok = true;
    // ---- Alignment. Candidate 1: cross-correlation around the first loud passage of A within
    // +/- min(2 s, 65536) samples (files started together). Then coarse offsets from the whole of
    // both files' decimated envelopes, each refined to the sample. The best-correlating wins.
    const int N = 1 << 17;
    Fine best;
    {
        const int maxLag = int(juce::jmin(2.0 * r.rateA, 65536.0));
        juce::int64 startA = 0;
        juce::AudioBuffer<float> scan(2, 65536);
        const float thresh = juce::Decibels::decibelsToGain(-40.0f);
        bool found = false;
        for (juce::int64 p = 0; p < A->lengthInSamples && !found; p += 65536) {
            int n = int(juce::jmin<juce::int64>(65536, A->lengthInSamples - p));
            A->read(&scan, 0, n, p, true, true);
            for (int i = 0; i < n; ++i)
                if (std::abs(scan.getSample(0, i)) > thresh) { startA = juce::jmax<juce::int64>(0, p + i - juce::int64(r.rateA * 0.25)); found = true; break; }
        }
        best = fineAlign(*A, *B, startA, 0, maxLag, N);
    }
    {
        int D = 1024;   // long enough to smooth over the waveform itself; at most 2^21 blocks
        const juce::int64 longest = juce::jmax(r.lengthA, r.lengthB);
        while (longest / D > (juce::int64(1) << 21)) D <<= 1;
        std::vector<double> ea, eb;
        if (!envelope(*A, D, ea, [&stage](double f) { return stage(0.1 * f); })) { ok = false; return best; }
        if (!envelope(*B, D, eb, [&stage](double f) { return stage(0.1 + 0.1 * f); })) { ok = false; return best; }
        for (const auto& c : coarseCandidates(ea, eb, D, N)) {
            auto f = fineAlign(*A, *B, c.startA, c.centre, c.maxLag, N);
            if (f.score > best.score) best = f;
        }
    }
    return best;
}

NullResult nullTest(juce::AudioFormatManager& fm, const juce::File& fa, const juce::File& fb, const juce::File& diffFile,
                    const std::function<bool(double)>& progress) {
    NullResult r;
    std::unique_ptr<juce::AudioFormatReader> A(fm.createReaderFor(fa)), B(fm.createReaderFor(fb));
    if (!A || !B) { r.error = "Couldn't read " + (!A ? fa.getFileName() : fb.getFileName()); return r; }
    r.rateA = A->sampleRate;
    r.rateB = B->sampleRate;
    r.bitsA = int(A->bitsPerSample);
    r.bitsB = int(B->bitsPerSample);
    r.lengthA = A->lengthInSamples;
    r.lengthB = B->lengthInSamples;
    if (std::abs(r.rateA - r.rateB) > 0.5) {
        r.error = "The files are at different sample rates (" + juce::String(r.rateA) + " vs " + juce::String(r.rateB) +
                  " Hz). Render both at the same rate to compare them sample for sample.";
        return r;
    }
    auto stage = [&progress](double p) { return !progress || progress(p); };
    auto cancelled = [&r] { r.error = "Cancelled."; return r; };

    bool alignedOk = true;
    const Fine best = findAlignment(*A, *B, stage, alignedOk);
    if (!alignedOk) return cancelled();
    r.offset = best.offset;
    r.inverted = best.inverted;
    if (!stage(0.2)) return cancelled();

    // ---- Sample-by-sample comparison over the overlap, noting where the files differ and
    // re-aligning where one of them dropped or gained samples.
    const juce::int64 from = juce::jmax<juce::int64>(0, -r.offset);
    if (juce::jmin(r.lengthA, r.lengthB - r.offset) <= from) { r.error = "The files don't overlap after alignment."; return r; }
    std::unique_ptr<juce::AudioFormatWriter> w;
    if (diffFile != juce::File()) {
        diffFile.deleteFile();
        juce::WavAudioFormat wav;
        if (auto os = diffFile.createOutputStream()) {
            w.reset(wav.createWriterFor(os.get(), r.rateA, 2, 32, {}, 0));
            if (w) os.release();
        }
    }
    const int blk = 65536;
    const double thr = 1e-5;                 // a difference this big starts an event
    const juce::int64 mergeGap = 64;         // events closer than this are one
    const juce::int64 settled = 2048;        // agreement needed before looking for a shift
    const int maxFailedSearches = 256;
    juce::AudioBuffer<float> ba(2, blk), bb(2, blk), bd(2, blk);
    double sumAA = 0, sumBB = 0, sumAB = 0, sumDD = 0;
    const double pol = r.inverted ? -1.0 : 1.0;
    std::vector<Event> events;
    Event ev;
    bool open = false;
    juce::int64 lastDiff = from - 1 - settled;
    juce::int64 diffPositions = 0;
    int failedSearches = 0;
    juce::int64 off = r.offset;
    const double span = double(juce::jmax<juce::int64>(1, r.lengthA - from));
    auto closeEvent = [&] {
        if (!open) return;
        open = false;
        ++r.differenceCount;
        if (events.size() < size_t(NullResult::maxListedDifferences)) events.push_back(ev);
    };
    auto search = [&](juce::int64 pos) {
        if (failedSearches >= maxFailedSearches) return 0;
        bool failed = false;
        const int k = findShift(*A, *B, pos, off, pol, failed);
        if (failed) ++failedSearches;
        return k;
    };

    juce::int64 p = from;
    while (true) {
        const juce::int64 to = juce::jmin(r.lengthA, r.lengthB - off);
        if (p >= to) break;
        const int n = int(juce::jmin<juce::int64>(blk, to - p));
        A->read(&ba, 0, n, p, true, true);
        B->read(&bb, 0, n, p + off, true, true);
        if (A->numChannels == 1) ba.copyFrom(1, 0, ba, 0, 0, n);
        if (B->numChannels == 1) bb.copyFrom(1, 0, bb, 0, 0, n);
        const float* xl = ba.getReadPointer(0);
        const float* xr = ba.getReadPointer(1);
        const float* yl = bb.getReadPointer(0);
        const float* yr = bb.getReadPointer(1);
        float* dl = bd.getWritePointer(0);
        float* dr = bd.getWritePointer(1);
        int done = n, shift = 0;
        for (int i = 0; i < n; ++i) {
            const juce::int64 pos = p + i;
            const double al = xl[i], ar = xr[i], bl = pol * yl[i], br = pol * yr[i];
            const double dL = al - bl, dR = ar - br;
            const double d = juce::jmax(std::abs(dL), std::abs(dR));
            if (d > thr) {
                if (!(open && pos - ev.last <= mergeGap)) {
                    closeEvent();
                    if (pos - lastDiff > settled) shift = search(pos);
                    open = true;
                    ev = Event();
                    ev.start = ev.last = pos;
                    ev.peak = d;
                    ev.off = off;
                    ev.dropK = shift;
                    if (shift != 0) { lastDiff = pos; done = i; break; }
                } else if (ev.dropK == 0 && ev.retries < 2 && pos - ev.start >= (ev.retries == 0 ? 4096 : 32768)) {
                    // A long event: maybe a drop with a glitch longer than the first look allowed for.
                    ++ev.retries;
                    shift = search(pos);
                    if (shift != 0) {
                        ev.dropK = shift;
                        ev.last = pos;
                        ev.peak = juce::jmax(ev.peak, d);
                        lastDiff = pos;
                        done = i;
                        break;
                    }
                }
                ev.last = pos;
                ev.peak = juce::jmax(ev.peak, d);
                lastDiff = pos;
                ++diffPositions;
            } else {
                r.residualPeakOutside = juce::jmax(r.residualPeakOutside, d);
            }
            sumAA += al * al + ar * ar;
            sumBB += bl * bl + br * br;
            sumAB += al * bl + ar * br;
            sumDD += dL * dL + dR * dR;
            r.peakA = juce::jmax(r.peakA, std::abs(al), std::abs(ar));
            r.peakB = juce::jmax(r.peakB, std::abs(bl), std::abs(br));
            r.residualPeak = juce::jmax(r.residualPeak, d);
            if (dL == 0.0) ++r.identical;
            if (dR == 0.0) ++r.identical;
            dl[i] = float(dL);
            dr[i] = float(dR);
        }
        if (w && done > 0) w->writeFromAudioSampleBuffer(bd, 0, done);
        r.compared += done;
        p += done;
        if (shift != 0) {
            off += shift;
            if (shift < 0) {
                // A's next -shift samples are missing from B: nothing to compare them with. The
                // difference file keeps A's timeline, so they go into it as they are in A.
                const int skip = int(juce::jmin<juce::int64>(-shift, r.lengthA - p));
                if (w && skip > 0) {
                    A->read(&bd, 0, skip, p, true, true);
                    if (A->numChannels == 1) bd.copyFrom(1, 0, bd, 0, 0, skip);
                    w->writeFromAudioSampleBuffer(bd, 0, skip);
                }
                p += juce::jmax(0, skip);
                ev.last = juce::jmax(ev.last, p - 1);
                lastDiff = ev.last;
            }
        }
        if (!stage(0.2 + 0.77 * juce::jmin(1.0, double(p - from) / span))) return cancelled();
    }
    closeEvent();

    if (r.compared > 0) {
        const double count = double(r.compared) * 2.0;
        r.signalRms = std::sqrt(sumAA / count);
        r.residualRms = std::sqrt(sumDD / count);
        if (sumBB > 0) {
            double g = sumAB / sumBB;
            r.gainDiffDb = g > 0 ? 20.0 * std::log10(g) : 0.0;
            double res = juce::jmax(0.0, sumAA - 2 * g * sumAB + g * g * sumBB);
            r.residualRmsGainMatched = std::sqrt(res / count);
        }
    }

    // ---- What kind of difference each event is. Broad differences everywhere (a level change,
    // different processing) aren't a list of spots, so they aren't listed.
    const double differingFraction = double(diffPositions) / double(juce::jmax<juce::int64>(1, r.compared));
    r.differsThroughout = r.residualRms > r.signalRms * 1e-4 && differingFraction > 0.1;
    if (r.differsThroughout) {
        r.differenceCount = 0;
        r.residualPeakOutside = 0;
    } else {
        for (size_t e = 0; e < events.size(); ++e) {
            const Event& x = events[e];
            NullResult::Difference d;
            d.start = x.start;
            d.length = x.last - x.start + 1;
            d.peak = x.peak;
            if (x.dropK != 0) {
                const int k = std::abs(x.dropK);
                d.length = juce::jmax<juce::int64>(d.length, k);
                d.kind = "dropped samples (" + juce::String(k) + (k == 1 ? " sample)" : " samples)");
                d.inFile = x.dropK < 0 ? "B" : "A";
            } else {
                juce::int64 lag = 0;
                if (d.length <= (1 << 17)) {
                    lag = repeatLag(*A, x.start, int(d.length));
                    if (lag != 0) d.inFile = "A";
                    else if ((lag = repeatLag(*B, x.start + x.off, int(d.length))) != 0) d.inFile = "B";
                }
                if (lag != 0) d.kind = "repeated audio (" + juce::String(lag) + (lag == 1 ? " sample back)" : " samples back)");
                else if (d.length < 4096) {
                    d.kind = "click";
                    d.inFile = clickFile(*A, *B, x.start, x.off, int(d.length));
                } else {
                    d.kind = "differs";
                    d.inFile = "?";
                }
            }
            r.differences.add(d);
            if (!stage(0.97 + 0.03 * double(e + 1) / double(events.size()))) return cancelled();
        }
    }
    r.ok = true;
    return r;
}

juce::String NullResult::toText(const juce::String& nameA, const juce::String& nameB) const {
    juce::String s;
    if (!ok) return "Null test failed: " + error;
    s << "NULL TEST\n";
    s << "  A: " << nameA << "  (" << juce::String(rateA) << " Hz, " << bitsA << "-bit, " << juce::String(double(lengthA) / rateA, 2) << " s)\n";
    s << "  B: " << nameB << "  (" << juce::String(rateB) << " Hz, " << bitsB << "-bit, " << juce::String(double(lengthB) / rateB, 2) << " s)\n\n";
    s << "Alignment\n";
    s << "  Offset:    B is " << juce::String(std::abs(offset)) << " samples " << (offset >= 0 ? "later" : "earlier") << " than A ("
      << juce::String(1000.0 * double(std::abs(offset)) / rateA, 3) << " ms)\n";
    s << "  Polarity:  " << (inverted ? "INVERTED" : "same") << "\n";
    s << "  Compared:  " << juce::String(double(compared) / rateA, 2) << " s\n\n";
    s << "Levels\n";
    s << "  Peak A " << db(peakA) << ", peak B " << db(peakB) << "\n";
    s << "  Best gain match: B needs " << juce::String(gainDiffDb, 4) << " dB\n\n";
    s << "Residual (A - B)\n";
    s << "  RMS:   " << db(residualRms) << "   (null depth " << (signalRms > 0 && residualRms > 0 ? juce::String(20 * std::log10(residualRms / signalRms), 1) + " dB" : juce::String("total")) << ")\n";
    s << "  Peak:  " << db(residualPeak) << "\n";
    s << "  After gain match, RMS: " << db(residualRmsGainMatched) << "\n";
    s << "  Identical samples: " << juce::String(100.0 * double(identical) / juce::jmax(1.0, double(compared) * 2), 3) << " %\n\n";

    bool anyDrop = false, allShort = true;
    for (const auto& d : differences) {
        if (d.kind.startsWith("dropped")) anyDrop = true;
        if (d.kind == "differs") allShort = false;
    }
    s << "Verdict\n";
    double resDb = residualRms > 0 ? 20 * std::log10(residualRms) : -999;
    if (!differsThroughout && differenceCount > 0)
        s << "  " << (residualPeakOutside > 0 ? "Identical to within " + db(residualPeakOutside) + " except for " : juce::String("Identical except for "))
          << juce::String(differenceCount) << (allShort ? " short" : " localized") << (differenceCount == 1 ? " event" : " events") << " listed below.\n";
    else if (residualPeak == 0.0) s << "  BIT-IDENTICAL. The two files contain exactly the same audio.\n";
    else if (resDb < -138) s << "  Identical within 24-bit precision. Any difference heard is not in the files.\n";
    else if (resDb < -90) s << "  Differ only at dither/word-length level (consistent with different dither or rounding).\n";
    else if (residualRmsGainMatched < residualRms * 0.1) s << "  Mostly a level difference of " << juce::String(gainDiffDb, 3) << " dB; after matching gain the residual is small.\n";
    else s << "  Audibly different processing (EQ, dynamics, SRC filter, timing or fades). Listen to the difference file.\n";
    if (std::abs(offset) > 0) s << "  Note: a pure delay was removed before comparing. Sub-sample timing differences (e.g. from SRC) won't null fully.\n";
    if (anyDrop) s << "  Note: where samples were dropped, the comparison re-aligned and carried on from there.\n";

    s << "\nDifferences\n";
    if (differsThroughout)
        s << "  The files differ throughout, so there are no separate spots to list.\n";
    else if (differences.isEmpty())
        s << (residualPeak == 0.0 ? "  None: the files agree everywhere they overlap.\n"
                                  : "  None above -100 dBFS: what differs is spread thinly and evenly through the files.\n");
    else {
        for (const auto& d : differences) {
            s << "  " << clock(rateA > 0 ? double(d.start) / rateA : 0.0) << "  " << d.kind;
            if (d.inFile != "?") s << " in " << d.inFile;
            s << ", " << juce::String(d.length) << (d.length == 1 ? " sample" : " samples") << ", peak " << db(d.peak) << "\n";
        }
        if (differenceCount > differences.size())
            s << "  ... and " << juce::String(differenceCount - differences.size()) << " more (" << juce::String(differenceCount) << " in all).\n";
    }
    return s;
}

AlignResult alignFiles(juce::AudioFormatManager& fm, const juce::File& a, const juce::File& b, const std::function<bool(double)>& progress) {
    AlignResult r;
    std::unique_ptr<juce::AudioFormatReader> A(fm.createReaderFor(a)), B(fm.createReaderFor(b));
    if (!A || !B) { r.error = "Can't read " + (A ? b : a).getFileName() + "."; return r; }
    if (std::abs(A->sampleRate - B->sampleRate) > 0.5) {
        r.error = a.getFileName() + " is " + juce::String(A->sampleRate / 1000.0, 1) + " kHz and " + b.getFileName() + " is " +
                  juce::String(B->sampleRate / 1000.0, 1) + " kHz: they can only be lined up at the same sample rate.";
        return r;
    }
    bool ok = true;
    const Fine f = findAlignment(*A, *B, [&progress](double p) { return !progress || progress(p * 5.0); }, ok);
    if (!ok) { r.error = "Cancelled."; return r; }
    r.ok = true;
    r.offset = f.offset;
    r.inverted = f.inverted;
    r.score = f.score;
    r.rate = A->sampleRate;
    return r;
}

}  // namespace lathe
