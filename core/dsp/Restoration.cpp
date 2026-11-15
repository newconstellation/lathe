#include "Restoration.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lathe::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
using cplx = std::complex<double>;

size_t frames(const Channels& ch) { return ch.empty() ? 0 : ch[0].size(); }

// ---------------------------------------------------------------- STFT helper
// sqrt-Hann analysis and synthesis at 75 % overlap: perfect reconstruction when unmodified.
struct Stft {
    int n, hop;
    std::vector<double> win;
    explicit Stft(int size) : n(size), hop(size / 4), win(size_t(size)) {
        for (int i = 0; i < n; ++i) win[size_t(i)] = std::sqrt(0.5 - 0.5 * std::cos(2 * kPi * i / n));
    }
    int numFrames(size_t len) const { return int((len + size_t(n)) / size_t(hop)) + 1; }
    // Frame f starts at sample f*hop - n (so the first frames cover the start fully).
    void analyse(const std::vector<double>& x, int f, std::vector<cplx>& out) const {
        out.assign(size_t(n), 0.0);
        long start = long(f) * hop - n;
        for (int i = 0; i < n; ++i) {
            long k = start + i;
            if (k >= 0 && k < long(x.size())) out[size_t(i)] = x[size_t(k)] * win[size_t(i)];
        }
        fft(out, false);
    }
    void synthAdd(std::vector<cplx>& spec, int f, std::vector<double>& acc) const {
        fft(spec, true);
        long start = long(f) * hop - n;
        for (int i = 0; i < n; ++i) {
            long k = start + i;
            if (k >= 0 && k < long(acc.size())) acc[size_t(k)] += spec[size_t(i)].real() * win[size_t(i)] * 0.5;   // hann sums to 2 at 75 %
        }
    }
};

// ---------------------------------------------------------------- zero-phase biquad
struct Biq {
    double b0, b1, b2, a1, a2;
    void run(std::vector<double>& x) const {
        double z1 = 0, z2 = 0;
        for (auto& v : x) {
            double y = b0 * v + z1;
            z1 = b1 * v - a1 * y + z2;
            z2 = b2 * v - a2 * y;
            v = y;
        }
    }
    void filtfilt(std::vector<double>& x) const {
        run(x);
        std::reverse(x.begin(), x.end());
        run(x);
        std::reverse(x.begin(), x.end());
    }
};

Biq notch(double f, double rate, double q) {
    double w = 2 * kPi * f / rate, alpha = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + alpha;
    return {1 / a0, -2 * c / a0, 1 / a0, -2 * c / a0, (1 - alpha) / a0};
}

// ---------------------------------------------------------------- AR modelling
// Autocorrelation-method LPC via Levinson-Durbin. a[0] = 1.
std::vector<double> lpc(const double* x, int n, int order) {
    std::vector<double> r(size_t(order + 1), 0.0);
    for (int k = 0; k <= order; ++k)
        for (int i = k; i < n; ++i) r[size_t(k)] += x[i] * x[i - k];
    std::vector<double> a(size_t(order + 1), 0.0);
    a[0] = 1;
    if (r[0] <= 1e-20) return a;
    r[0] *= 1.0 + 1e-9;   // slight regularisation
    double err = r[0];
    for (int i = 1; i <= order; ++i) {
        double acc = r[size_t(i)];
        for (int j = 1; j < i; ++j) acc += a[size_t(j)] * r[size_t(i - j)];
        double k = -acc / err;
        std::vector<double> prev(a);
        for (int j = 1; j < i; ++j) a[size_t(j)] = prev[size_t(j)] + k * prev[size_t(i - j)];
        a[size_t(i)] = k;
        err *= (1 - k * k);
        if (err <= 1e-20) break;
    }
    return a;
}

// Solve the symmetric positive system M x = b (Cholesky); M is n x n row-major.
bool cholSolve(std::vector<double>& M, std::vector<double>& b, int n) {
    for (int j = 0; j < n; ++j) {
        double s = M[size_t(j * n + j)];
        for (int k = 0; k < j; ++k) s -= M[size_t(j * n + k)] * M[size_t(j * n + k)];
        if (s <= 1e-18) return false;
        double d = std::sqrt(s);
        M[size_t(j * n + j)] = d;
        for (int i = j + 1; i < n; ++i) {
            double t = M[size_t(i * n + j)];
            for (int k = 0; k < j; ++k) t -= M[size_t(i * n + k)] * M[size_t(j * n + k)];
            M[size_t(i * n + j)] = t / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double t = b[size_t(i)];
        for (int k = 0; k < i; ++k) t -= M[size_t(i * n + k)] * b[size_t(k)];
        b[size_t(i)] = t / M[size_t(i * n + i)];
    }
    for (int i = n - 1; i >= 0; --i) {
        double t = b[size_t(i)];
        for (int k = i + 1; k < n; ++k) t -= M[size_t(k * n + i)] * b[size_t(k)];
        b[size_t(i)] = t / M[size_t(i * n + i)];
    }
    return true;
}

// Least-squares AR interpolation of x[g0, g1) from the surrounding context.
void lsarInterpolate(std::vector<double>& x, long g0, long g1, int order) {
    const long L = g1 - g0;
    const long ctx = std::max<long>(order * 4, L * 3);
    long c0 = std::max<long>(0, g0 - ctx), c1 = std::min<long>(long(x.size()), g1 + ctx);
    if (g0 - c0 < order || c1 - g1 < order) return;   // not enough context
    // AR model from the clean context either side (gap zeroed so it doesn't bias the estimate).
    std::vector<double> seg(x.begin() + c0, x.begin() + c1);
    for (long i = g0; i < g1; ++i) seg[size_t(i - c0)] = 0;
    auto a = lpc(seg.data(), int(seg.size()), order);
    // Minimise sum_n (sum_k a_k x[n-k])^2 over n in [c0+order, c1) w.r.t. the gap samples.
    const int n = int(L);
    std::vector<double> M(size_t(n * n), 0.0), rhs(size_t(n), 0.0);
    for (long t = c0 + order; t < c1; ++t) {
        // e(t) = sum_k a_k x[t-k] = known part + sum over unknown u: a_{t-u} x_u
        double known = 0;
        int ui[64];
        double uc[64];
        int nu = 0;
        for (int k = 0; k <= order; ++k) {
            long s = t - k;
            if (s >= g0 && s < g1) {
                if (nu < 64) { ui[nu] = int(s - g0); uc[nu] = a[size_t(k)]; ++nu; }
            } else {
                known += a[size_t(k)] * x[size_t(s)];
            }
        }
        for (int p = 0; p < nu; ++p) {
            rhs[size_t(ui[p])] -= uc[p] * known;
            for (int q2 = 0; q2 < nu; ++q2) M[size_t(ui[p] * n + ui[q2])] += uc[p] * uc[q2];
        }
    }
    for (int i = 0; i < n; ++i) M[size_t(i * n + i)] += 1e-9;
    if (cholSolve(M, rhs, n))
        for (int i = 0; i < n; ++i) x[size_t(g0 + i)] = rhs[size_t(i)];
}

// Autocorrelation-method LPC from two separate stretches (the audio either side of a gap), so
// nothing inside the gap biases the model. a[0] = 1.
std::vector<double> lpcTwoSided(const double* left, int nl, const double* right, int nr, int order) {
    std::vector<double> r(size_t(order + 1), 0.0);
    for (int k = 0; k <= order; ++k) {
        double acc = 0;
        for (int i = k; i < nl; ++i) acc += left[i] * left[i - k];
        for (int i = k; i < nr; ++i) acc += right[i] * right[i - k];
        r[size_t(k)] = acc;
    }
    std::vector<double> a(size_t(order + 1), 0.0);
    a[0] = 1;
    if (r[0] <= 1e-20) return a;
    r[0] *= 1.0 + 1e-7;
    double err = r[0];
    std::vector<double> prev;
    for (int i = 1; i <= order; ++i) {
        double acc = r[size_t(i)];
        for (int j = 1; j < i; ++j) acc += a[size_t(j)] * r[size_t(i - j)];
        const double k = -acc / err;
        prev = a;
        for (int j = 1; j < i; ++j) a[size_t(j)] = prev[size_t(j)] + k * prev[size_t(i - j)];
        a[size_t(i)] = k;
        err *= (1 - k * k);
        if (err <= 1e-20 * r[0]) break;
    }
    return a;
}

// Least-squares AR interpolation of x[g0, g1) for gaps of any length: the normal equations are
// banded (half-width = order), so they are solved with a banded Cholesky.
bool lsarInterpolateBanded(std::vector<double>& x, long g0, long g1) {
    const long N = long(x.size()), L = g1 - g0;
    if (L <= 0 || g0 < 0 || g1 > N) return false;
    const long avail = std::min(g0, N - g1);
    // Short gaps need few coefficients; longer ones need enough to hold the low notes too.
    int p = L <= 32 ? 32 : int(std::clamp<long>(4 * L, 256, 512));
    p = int(std::min<long>(p, avail / 3));
    if (p < 4) return false;
    const long ctx = std::min<long>(avail, std::max<long>(long(p) * 16, 8192));
    const auto a = lpcTwoSided(x.data() + (g0 - ctx), int(ctx), x.data() + g1, int(ctx), p);
    // M[i][j] = sum_t a[t-u_i] a[t-u_j] over t in [g0, g1 + p): a Toeplitz band, ra[|i-j|].
    std::vector<double> ra(size_t(p + 1), 0.0);
    for (int d = 0; d <= p; ++d)
        for (int k = 0; k + d <= p; ++k) ra[size_t(d)] += a[size_t(k)] * a[size_t(k + d)];
    const int n = int(L), w = std::min(p, n - 1);
    std::vector<double> rhs(size_t(n), 0.0);
    for (long t = g0; t < g1 + p; ++t) {
        double known = 0;
        for (int k = 0; k <= p; ++k) {
            const long s = t - k;
            if (s < g0 || s >= g1) known += a[size_t(k)] * x[size_t(s)];
        }
        const long u0 = std::max(g0, t - p), u1 = std::min(g1 - 1, t);
        for (long u = u0; u <= u1; ++u) rhs[size_t(u - g0)] -= a[size_t(t - u)] * known;
    }
    // Banded Cholesky, lower band: B[i][d] = L[i][i-d], d = 0..w.
    std::vector<double> B(size_t(n) * size_t(w + 1), 0.0);
    auto at = [&](int i, int d) -> double& { return B[size_t(i) * size_t(w + 1) + size_t(d)]; };
    const double ridge = ra[0] * 1e-9;
    for (int i = 0; i < n; ++i) {
        for (int d = std::min(i, w); d >= 0; --d) {
            const int j = i - d;
            double sum = ra[size_t(d)] + (d == 0 ? ridge : 0.0);
            const int k0 = std::max(0, i - w);
            for (int k = k0; k < j; ++k) sum -= at(i, i - k) * at(j, j - k);
            if (d == 0) {
                if (sum <= 0) return false;
                at(i, 0) = std::sqrt(sum);
            } else {
                at(i, d) = sum / at(j, 0);
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        double t = rhs[size_t(i)];
        for (int k = std::max(0, i - w); k < i; ++k) t -= at(i, i - k) * rhs[size_t(k)];
        rhs[size_t(i)] = t / at(i, 0);
    }
    for (int i = n - 1; i >= 0; --i) {
        double t = rhs[size_t(i)];
        for (int k = i + 1; k <= std::min(n - 1, i + w); ++k) t -= at(k, k - i) * rhs[size_t(k)];
        rhs[size_t(i)] = t / at(i, 0);
    }
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(rhs[size_t(i)])) return false;
    for (int i = 0; i < n; ++i) x[size_t(g0 + i)] = rhs[size_t(i)];
    return true;
}

struct Gap { long g0, g1; double strength; };

// One detection pass: stretches whose prediction residual stands far above the local level.
std::vector<Gap> detectGaps(const Channels& ch, double rate, double threshold, int order) {
    std::vector<Gap> gaps;
    const size_t len = frames(ch);
    if (len < size_t(order * 8)) return gaps;
    const int block = int(std::max(2048.0, rate * 0.05));
    // Joint detector: residual energy summed over channels.
    std::vector<double> resid(len, 0.0);
    for (auto& x : ch) {
        for (size_t b0 = 0; b0 < len; b0 += size_t(block)) {
            size_t b1 = std::min(len, b0 + size_t(block));
            size_t w0 = b0 >= size_t(order) ? b0 - size_t(order) : 0;
            auto a = lpc(x.data() + w0, int(b1 - w0), order);
            for (size_t t = std::max(b0, size_t(order)); t < b1; ++t) {
                double e = 0;
                for (int k = 0; k <= order; ++k) e += a[size_t(k)] * x[t - size_t(k)];
                resid[t] += e * e;
            }
        }
    }
    // Robust local scale: median of |e| per block.
    std::vector<double> scale((len + size_t(block) - 1) / size_t(block), 0.0);
    for (size_t b = 0; b < scale.size(); ++b) {
        size_t b0 = b * size_t(block), b1 = std::min(len, b0 + size_t(block));
        std::vector<double> v(resid.begin() + long(b0), resid.begin() + long(b1));
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
        scale[b] = std::sqrt(std::max(1e-24, v[v.size() / 2]));
    }
    // Mark samples whose residual exceeds threshold x local scale, widen, merge.
    long cur0 = -1, last = -1;
    double peak = 0;
    for (size_t t = 0; t < len; ++t) {
        const double limit = threshold * scale[t / size_t(block)] * 1.4826;
        const double e = std::sqrt(resid[t]);
        if (e > limit) {
            if (cur0 < 0) { cur0 = long(t); peak = 0; }
            last = long(t);
            peak = std::max(peak, e / limit);
        } else if (cur0 >= 0 && long(t) - last > 16) {
            // A click's tail keeps exciting the predictor a few samples after the spike:
            // repair a little beyond the last flagged sample, and merge flags < 16 apart.
            gaps.push_back({cur0 - 3, last + 6, peak});
            cur0 = -1;
        }
    }
    if (cur0 >= 0) gaps.push_back({cur0 - 3, last + 6, peak});
    for (auto& g : gaps) {
        g.g0 = std::max<long>(0, g.g0);
        g.g1 = std::min<long>(long(len), g.g1);
    }
    return gaps;
}

ClickStats detectAndRepair(Channels& ch, double rate, double threshold, int maxLen, int order, int passes) {
    ClickStats st;
    for (int pass = 0; pass < passes; ++pass) {
        int fixedThisPass = 0;
        for (auto& g : detectGaps(ch, rate, threshold, order)) {
            if (g.g1 - g.g0 > maxLen || g.g1 <= g.g0) continue;
            for (auto& x : ch) lsarInterpolate(x, g.g0, g.g1, order);
            ++fixedThisPass;
            st.samplesRepaired += g.g1 - g.g0;
        }
        st.found += fixedThisPass;
        if (fixedThisPass == 0) break;
    }
    return st;
}

// Zero-phase low-pass: a second-order Butterworth run forwards and backwards, twice.
std::vector<double> lowpassZeroPhase(const std::vector<double>& x, double rate, double fc) {
    const double k = std::tan(3.14159265358979323846 * fc / rate), q = 0.70710678118654752;
    const double norm = 1.0 / (1.0 + k / q + k * k);
    const double b0 = k * k * norm, b1 = 2 * b0, b2 = b0, a1 = 2 * (k * k - 1) * norm, a2 = (1 - k / q + k * k) * norm;
    std::vector<double> y(x);
    auto run = [&](bool reverse) {
        // Start in steady state at the first value, so the ends don't ring.
        const long n = long(y.size());
        if (n == 0) return;
        const double x0 = y[size_t(reverse ? n - 1 : 0)];
        double z1 = x0 * (1 - b0), z2 = x0 * (b2 - a2);
        for (long i = 0; i < n; ++i) {
            double& v = y[size_t(reverse ? n - 1 - i : i)];
            const double in = v, out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            v = out;
        }
    };
    for (int pass = 0; pass < 2; ++pass) { run(false); run(true); }
    return y;
}
}  // namespace

// ---------------------------------------------------------------- FFT

void fft(std::vector<cplx>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * kPi / double(len) * (inverse ? 1 : -1);
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

int stftSizeFor(double rate) {
    int n = 4096;
    while (rate > 50000.0 * double(n) / 4096.0 * 1.0 && n < 16384) n *= 2;
    return n;
}

// ---------------------------------------------------------------- DC / hum

void removeDc(Channels& ch) {
    for (auto& x : ch) {
        if (x.empty()) continue;
        double m = std::accumulate(x.begin(), x.end(), 0.0) / double(x.size());
        for (auto& v : x) v -= m;
    }
}

double removeHum(Channels& ch, double rate, double f0, int harmonics, double q) {
    if (f0 <= 0) {
        // Detect: which of 50 / 60 Hz (and their 2nd harmonic) stands out more from the spectrum?
        const int n = 1 << 16;
        std::vector<cplx> s(size_t(n), 0.0);
        size_t len = frames(ch);
        for (size_t i = 0; i < std::min(len, size_t(n)); ++i) {
            double v = 0;
            for (auto& x : ch) v += x[i];
            s[i] = v * (0.5 - 0.5 * std::cos(2 * kPi * double(i) / n));
        }
        fft(s, false);
        auto peakness = [&](double f) {
            double binHz = rate / n, total = 0;
            for (int h = 1; h <= 2; ++h) {
                int b = int(std::round(f * h / binHz));
                double at = std::abs(s[size_t(b)]);
                double around = 0;
                for (int k = 6; k <= 20; ++k) around += std::abs(s[size_t(b - k)]) + std::abs(s[size_t(b + k)]);
                around /= 30.0;
                total += at / std::max(1e-12, around);
            }
            return total;
        };
        double p50 = peakness(50), p60 = peakness(60);
        if (std::max(p50, p60) < 6.0) return 0;   // no clear hum
        f0 = p60 > p50 ? 60.0 : 50.0;
    }
    for (auto& x : ch)
        for (int h = 1; h <= harmonics; ++h) {
            double f = f0 * h;
            if (f >= rate * 0.45) break;
            notch(f, rate, q * h).filtfilt(x);   // narrower notches for higher harmonics
        }
    return f0;
}

// ---------------------------------------------------------------- clicks

ClickStats declick(Channels& ch, double rate, double sensitivity, double maxClickMs) {
    double threshold = 14.0 - std::clamp(sensitivity, 1.0, 10.0);   // 13 (gentle) .. 4 (sensitive)
    int maxLen = int(std::clamp(maxClickMs, 0.1, 5.0) * rate / 1000.0);
    int order = int(std::clamp(rate / 1500.0, 20.0, 64.0));
    return detectAndRepair(ch, rate, threshold, std::min(maxLen, 200), order, 2);
}

std::vector<ClickEvent> findClicks(const Channels& ch, double rate, double sensitivity, double maxClickMs) {
    const double threshold = 14.0 - std::clamp(sensitivity, 1.0, 10.0);
    const int maxLen = std::min(200, int(std::clamp(maxClickMs, 0.1, 5.0) * rate / 1000.0));
    const int order = int(std::clamp(rate / 1500.0, 20.0, 64.0));
    std::vector<ClickEvent> out;
    for (auto& g : detectGaps(ch, rate, threshold, order))
        if (g.g1 > g.g0 && g.g1 - g.g0 <= maxLen) out.push_back({g.g0, g.g1 - g.g0, g.strength});
    return out;
}

int repairClicks(Channels& ch, const std::vector<ClickEvent>& events) {
    int n = 0;
    for (auto& e : events)
        if (interpolateGap(ch, e.start, e.start + e.length)) ++n;
    return n;
}

bool interpolateGap(Channels& ch, int64_t g0, int64_t g1) {
    const int64_t len = int64_t(frames(ch));
    if (g1 <= g0 || g0 < 0 || g1 > len || g1 - g0 > kMaxInterpolate) return false;
    Channels work = ch;
    for (auto& x : work)
        if (!lsarInterpolateBanded(x, long(g0), long(g1))) return false;
    ch = std::move(work);
    return true;
}

bool patchFromSimilar(Channels& ch, double rate, int64_t g0, int64_t g1, double* scoreOut) {
    if (scoreOut) *scoreOut = 0;
    const long N = long(frames(ch)), L = long(g1 - g0);
    if (L <= 0 || g0 < 0 || g1 > N) return false;
    const long C = std::clamp<long>(L, 1024, 8192);
    if (g0 < C || g1 + C > N) return false;
    // Mono sum for matching.
    std::vector<double> m(size_t(N), 0.0);
    for (auto& x : ch)
        for (long i = 0; i < N; ++i) m[size_t(i)] += x[size_t(i)];
    // Score of a candidate source start s: the audio around [s, s+L) against the audio around the gap.
    auto score = [&](long s, long step) {
        double ab = 0, aa = 0, bb = 0;
        for (long i = -C; i < 0; i += step) {
            const double t = m[size_t(g0 + i)], c = m[size_t(s + i)];
            ab += t * c; aa += t * t; bb += c * c;
        }
        for (long i = 0; i < C; i += step) {
            const double t = m[size_t(g1 + i)], c = m[size_t(s + L + i)];
            ab += t * c; aa += t * t; bb += c * c;
        }
        // Penalise a level mismatch as well as a shape mismatch.
        return aa > 0 && bb > 0 ? 2.0 * ab / (aa + bb) : 0.0;
    };
    auto valid = [&](long s) { return s - C >= 0 && s + L + C <= N && (s + L + C <= g0 || s - C >= g1); };
    // Coarse search on a grid, then refine around the best few.
    const long coarse = std::max<long>(1, long(rate / 12000.0));
    std::vector<std::pair<double, long>> best;
    for (long s = C; s + L + C <= N; s += coarse) {
        if (!valid(s)) continue;
        const double v = score(s, 4 * coarse);
        if (best.size() < 8) { best.push_back({v, s}); std::sort(best.begin(), best.end(), std::greater<>()); }
        else if (v > best.back().first) { best.back() = {v, s}; std::sort(best.begin(), best.end(), std::greater<>()); }
    }
    double bestV = 0;
    long bestS = -1;
    for (auto& b : best)
        for (long s = b.second - 2 * coarse; s <= b.second + 2 * coarse; ++s) {
            if (!valid(s)) continue;
            const double v = score(s, 1);
            if (v > bestV) { bestV = v; bestS = s; }
        }
    if (scoreOut) *scoreOut = bestV;
    if (bestS < 0 || bestV < 0.35) return false;
    // Copy with blends just outside the gap (the gap itself is fully replaced).
    const long F = std::min<long>(C / 2, std::max<long>(16, long(rate * 0.004)));
    for (auto& x : ch) {
        const std::vector<double> src(x);
        for (long i = -F; i < L + F; ++i) {
            double w = 1.0;
            if (i < 0) w = double(i + F) / double(F);
            else if (i >= L) w = double(L + F - i) / double(F);
            w = 0.5 - 0.5 * std::cos(3.14159265358979323846 * w);
            x[size_t(g0 + i)] = src[size_t(g0 + i)] * (1 - w) + src[size_t(bestS + i)] * w;
        }
    }
    return true;
}

bool dethump(Channels& ch, double rate, int64_t g0, int64_t g1, double cutoffHz) {
    const long N = long(frames(ch));
    if (g1 <= g0 || g0 < 0 || g1 > N) return false;
    const double fc = std::clamp(cutoffHz, 40.0, 400.0);
    const long D = std::max<long>(1, long(rate / (fc * 8.0)));
    // The low band of a thump rings beyond the marked stretch: rebuild a little more of it.
    const long spread = long(1.5 * rate / fc);
    const long a = long(g0) - spread, b = long(g1) + spread;
    const long da = a / D - 1, db = (b + D - 1) / D + 1;   // gap in decimated samples
    const long nd = N / D;
    if (a < 0 || b > N || da < 8 || db + 8 > nd) return false;
    Channels out = ch;
    for (size_t c = 0; c < ch.size(); ++c) {
        const auto low = lowpassZeroPhase(ch[c], rate, fc);
        std::vector<double> dec;
        dec.resize(size_t(nd));
        for (long i = 0; i < nd; ++i) dec[size_t(i)] = low[size_t(i * D)];
        if (!lsarInterpolateBanded(dec, da, db)) return false;
        // Back to the full rate (Catmull-Rom; the band is far below the decimated Nyquist).
        auto lowAt = [&](long i) {
            const double pos = double(i) / double(D);
            const long k = long(std::floor(pos));
            const double t = pos - double(k);
            auto v = [&](long j) { return dec[size_t(std::clamp<long>(j, 0, nd - 1))]; };
            const double p0 = v(k - 1), p1 = v(k), p2 = v(k + 1), p3 = v(k + 2);
            return p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
        };
        const long i0 = da * D, i1 = std::min(N, db * D), F = 2 * D;
        for (long i = i0; i < i1; ++i) {
            double w = 1.0;
            if (i - i0 < F) w = double(i - i0) / double(F);
            else if (i1 - i <= F) w = double(i1 - i) / double(F);
            out[c][size_t(i)] = ch[c][size_t(i)] + w * (lowAt(i) - low[size_t(i)]);
        }
    }
    ch = std::move(out);
    return true;
}

void scanGlitches(const Channels& win, size_t from, size_t to, int64_t base, std::vector<Glitch>& out) {
    const size_t N = frames(win);
    to = std::min(to, N);
    if (win.empty() || from + 8 >= to) return;
    const size_t nc = win.size();
    // Second difference, summed over channels: a step in the waveform stands far out of it.
    const size_t block = 2048;
    auto d2 = [&](size_t n) {
        double s = 0;
        for (auto& x : win) s += std::abs(x[n] - 2 * x[n - 1] + x[n - 2]);
        return s;
    };
    auto step = [&](size_t n) {
        double s = 0;
        for (auto& x : win) s = std::max(s, std::abs(x[n] - x[n - 1]));
        return s;
    };
    auto equalAt = [&](size_t n, size_t lag) {
        if (n < lag) return false;
        for (size_t c = 0; c < nc; ++c)
            if (win[c][n] != win[c][n - lag]) return false;
        return true;
    };
    auto silentAt = [&](size_t n) {
        for (size_t c = 0; c < nc; ++c)
            if (win[c][n] != 0.0) return false;
        return true;
    };
    std::vector<double> v;
    size_t skipUntil = 0;
    for (size_t b0 = std::max<size_t>(from, 2); b0 < to; b0 += block) {
        const size_t b1 = std::min(to, b0 + block);
        v.clear();
        for (size_t n = b0; n < b1; ++n) v.push_back(d2(n));
        std::vector<double> sorted(v);
        std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
        const double med = sorted[sorted.size() / 2];
        const double limit = std::max(25.0 * med, 1e-4);
        for (size_t n = b0; n < b1; ++n) {
            if (n < skipUntil || v[n - b0] <= limit) continue;
            // A candidate step at about n. Is what follows a copy of earlier audio?
            bool found = false;
            for (size_t s = n >= 3 ? n - 3 : 0; s <= n + 3 && s + 4 < N && !found; ++s) {
                for (int k = 6; k <= 18 && !found; ++k) {
                    const size_t lag = size_t(1) << k;
                    if (s < lag) break;
                    size_t run = 0;
                    bool anySound = false;
                    while (s + run < N && run < lag && equalAt(s + run, lag)) { anySound = anySound || !silentAt(s + run); ++run; }
                    if (run < 4 || !anySound) continue;
                    // It must end with a break as well (otherwise it's a held or periodic signal).
                    if (run >= lag) continue;
                    size_t start = s;
                    while (start > from && s - start < 8 && equalAt(start - 1, lag) && !silentAt(start - 1)) --start;
                    Glitch g;
                    g.start = base + int64_t(start);
                    g.length = int64_t(s + run - start);
                    g.lag = int64_t(lag);
                    g.jump = step(start);
                    out.push_back(g);
                    skipUntil = s + run + 8;
                    found = true;
                }
            }
            if (found) continue;
            // A plain break: only when it is far out of character and a real step in level.
            if (v[n - b0] > 80.0 * std::max(med, 1e-6) && step(n) > 0.05) {
                size_t end = n;
                while (end + 1 < b1 && end - n < 64 && v[end + 1 - b0] > limit) ++end;
                Glitch g;
                g.start = base + int64_t(n);
                g.length = int64_t(end - n + 1);
                g.jump = step(n);
                out.push_back(g);
                skipUntil = end + 64;
            }
        }
    }
}

ClickStats decrackle(Channels& ch, double rate, double amount) {
    double threshold = 7.0 - std::clamp(amount, 0.0, 10.0) * 0.35;   // 7 .. 3.5
    int order = int(std::clamp(rate / 2000.0, 16.0, 48.0));
    return detectAndRepair(ch, rate, threshold, int(rate * 0.0005) + 4, order, 3);
}

// ---------------------------------------------------------------- noise

NoiseProfile learnNoise(const Channels& ch, double rate) {
    NoiseProfile np;
    np.fftSize = stftSizeFor(rate);
    np.rate = rate;
    Stft st(np.fftSize);
    const size_t len = frames(ch);
    if (len < size_t(np.fftSize)) return np;
    np.power.assign(size_t(np.fftSize / 2 + 1), 0.0);
    int count = 0;
    std::vector<cplx> spec;
    // Use only frames fully inside the noise selection.
    for (int f = 4; long(f) * st.hop <= long(len); ++f) {
        for (auto& x : ch) {
            st.analyse(x, f, spec);
            for (size_t b = 0; b < np.power.size(); ++b) np.power[b] += std::norm(spec[b]);
        }
        ++count;
    }
    if (count == 0) { np.power.clear(); return np; }
    for (auto& p : np.power) p /= double(count) * double(ch.size());
    return np;
}

void denoise(Channels& ch, double rate, const NoiseProfile& np, double reductionDb, double sensitivity) {
    if (!np.valid() || ch.empty()) return;
    const int n = np.fftSize;
    Stft st(n);
    const size_t len = frames(ch), bins = size_t(n / 2 + 1);
    const double floorGain = std::pow(10.0, -std::abs(reductionDb) / 20.0);
    const double alpha = std::clamp(sensitivity, 0.3, 4.0);
    // Release over ~100 ms, attack fast: avoids the warbling "musical noise".
    const double framesPerSec = rate / st.hop;
    const double rel = std::exp(-1.0 / (0.10 * framesPerSec)), att = std::exp(-1.0 / (0.01 * framesPerSec));
    std::vector<double> gain(bins, 1.0), prevGain(bins, 1.0);
    std::vector<std::vector<double>> out(ch.size(), std::vector<double>(len, 0.0));
    std::vector<std::vector<cplx>> spec(ch.size());
    const int nf = st.numFrames(len);
    for (int f = 0; f < nf; ++f) {
        for (size_t c = 0; c < ch.size(); ++c) st.analyse(ch[c], f, spec[c]);
        for (size_t b = 0; b < bins; ++b) {
            double p = 0;
            for (size_t c = 0; c < ch.size(); ++c) p += std::norm(spec[c][b]);
            p /= double(ch.size());
            double noise = alpha * np.power[b];
            double g = p > 0 ? std::max(floorGain, 1.0 - noise / p) : floorGain;   // power-domain Wiener-like gain
            g = std::sqrt(std::max(g, floorGain * floorGain));
            gain[b] = g;
        }
        // Smooth across frequency (3 bins) then in time (attack/release).
        for (size_t b = 0; b < bins; ++b) {
            double s = gain[b] * 2;
            int k = 2;
            if (b > 0) { s += gain[b - 1]; ++k; }
            if (b + 1 < bins) { s += gain[b + 1]; ++k; }
            double g = s / k;
            double coeff = g > prevGain[b] ? att : rel;
            prevGain[b] = g + coeff * (prevGain[b] - g);
        }
        for (size_t c = 0; c < ch.size(); ++c) {
            for (size_t b = 0; b < bins; ++b) {
                spec[c][b] *= prevGain[b];
                if (b > 0 && b < size_t(n / 2)) spec[c][size_t(n) - b] = std::conj(spec[c][b]);
            }
            st.synthAdd(spec[c], f, out[c]);
        }
    }
    for (size_t c = 0; c < ch.size(); ++c) ch[c].swap(out[c]);
}

// ---------------------------------------------------------------- spectral editing

void spectralEdit(Channels& ch, double rate, double t0, double t1, double f0, double f1, SpectralMode mode, double gainDb) {
    if (ch.empty() || t1 <= t0 || f1 <= f0) return;
    const int n = stftSizeFor(rate);
    Stft st(n);
    const size_t len = frames(ch), bins = size_t(n / 2 + 1);
    const double binHz = rate / n;
    const size_t b0 = size_t(std::clamp(std::floor(f0 / binHz), 0.0, double(bins - 1)));
    const size_t b1 = size_t(std::clamp(std::ceil(f1 / binHz), double(b0 + 1), double(bins)));
    // Frames whose centre lies in [t0, t1). Frame f is centred at f*hop - n/2.
    auto frameAt = [&](double t) { return int(std::round((t * rate + n / 2.0) / st.hop)); };
    const int fa = std::max(0, frameAt(t0)), fb = std::max(fa + 1, frameAt(t1));
    // Only re-synthesise the affected span (plus overlap) so everything else stays bit-exact.
    const long s0 = std::max<long>(0, long(fa - 4) * st.hop - n), s1 = std::min<long>(long(len), long(fb + 4) * st.hop);
    const double g = std::pow(10.0, gainDb / 20.0);
    for (auto& x : ch) {
        std::vector<double> acc(len, 0.0);
        std::vector<cplx> spec, before, after;
        if (mode == SpectralMode::Heal) {
            st.analyse(x, std::max(0, fa - 2), before);
            st.analyse(x, fb + 1, after);
        }
        for (int f = std::max(0, fa - 4); f <= fb + 4; ++f) {
            st.analyse(x, f, spec);
            if (f >= fa && f < fb) {
                double w = fb > fa ? double(f - fa + 0.5) / double(fb - fa) : 0.5;
                for (size_t b = b0; b < b1; ++b) {
                    // Soft edges in frequency: 2-bin taper.
                    double edge = 1.0;
                    if (b < b0 + 2) edge = 0.5 + 0.25 * double(b - b0);
                    if (b + 2 >= b1) edge = std::min(edge, 0.5 + 0.25 * double(b1 - 1 - b));
                    if (mode == SpectralMode::Attenuate) {
                        spec[b] *= 1.0 + (g - 1.0) * edge;
                    } else {
                        double mag = std::abs(spec[b]);
                        double target = std::exp((1 - w) * std::log(std::abs(before[b]) + 1e-12) + w * std::log(std::abs(after[b]) + 1e-12));
                        double newMag = mag + (std::min(mag, target) - mag) * edge;
                        if (mag > 0) spec[b] *= newMag / mag;
                    }
                }
                for (size_t b = 1; b < size_t(n / 2); ++b) spec[size_t(n) - b] = std::conj(spec[b]);
            }
            st.synthAdd(spec, f, acc);
        }
        // Splice: inside the affected frames use the re-synthesis; crossfade over one hop at each end.
        const long e0 = std::max<long>(0, long(fa) * st.hop - n), e1 = std::min<long>(long(len), long(fb) * st.hop);
        for (long k = s0; k < s1; ++k) {
            double w = 1.0;
            if (k < e0) w = std::clamp(double(k - (e0 - st.hop)) / st.hop, 0.0, 1.0);
            else if (k >= e1) w = std::clamp(1.0 - double(k - e1) / st.hop, 0.0, 1.0);
            x[size_t(k)] = x[size_t(k)] * (1 - w) + acc[size_t(k)] * w;
        }
    }
}

Spectrogram spectrogram(const Channels& ch, double rate, int maxFrames) {
    Spectrogram sg;
    sg.rate = rate;
    sg.fftSize = std::max(1024, stftSizeFor(rate) / 2);
    const size_t len = frames(ch);
    if (len == 0) return sg;
    sg.hop = std::max(sg.fftSize / 4, int(len / size_t(std::max(1, maxFrames))) + 1);
    std::vector<double> win(size_t(sg.fftSize));
    for (int i = 0; i < sg.fftSize; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * kPi * i / sg.fftSize);
    std::vector<cplx> s;
    const double norm = 4.0 / sg.fftSize;
    for (size_t start = 0; start + size_t(sg.fftSize) <= len + size_t(sg.fftSize / 2); start += size_t(sg.hop)) {
        s.assign(size_t(sg.fftSize), 0.0);
        for (int i = 0; i < sg.fftSize; ++i) {
            size_t k = start + size_t(i);
            if (k >= len) break;
            double v = 0;
            for (auto& x : ch) v += x[k];
            s[size_t(i)] = v / double(ch.size()) * win[size_t(i)];
        }
        fft(s, false);
        std::vector<float> col(size_t(sg.fftSize / 2 + 1));
        for (size_t b = 0; b < col.size(); ++b) col[b] = float(20 * std::log10(std::abs(s[b]) * norm + 1e-10));
        sg.db.push_back(std::move(col));
    }
    return sg;
}

}  // namespace lathe::dsp
