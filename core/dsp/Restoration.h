// Offline restoration and spectral repair. All functions work on whole channel buffers
// (64-bit) at any sample rate; they're run on a copy of a region's audio and the result
// is written to a new file, so the original is never touched.
#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace lathe::dsp {

using Channels = std::vector<std::vector<double>>;

// In-place radix-2 FFT (size must be a power of two).
void fft(std::vector<std::complex<double>>& a, bool inverse);

// ---- DC and hum -----------------------------------------------------------------
void removeDc(Channels& ch);
// Zero-phase notches at f0 and its harmonics. f0 <= 0: detect 50 or 60 Hz automatically.
// Returns the fundamental used (0 if none was found when detecting).
double removeHum(Channels& ch, double rate, double f0, int harmonics, double q);

// ---- Clicks and crackle -----------------------------------------------------------
struct ClickStats {
    int found = 0;
    int64_t samplesRepaired = 0;
};
// sensitivity: 1 (only big clicks) .. 10 (very sensitive). maxClickMs: longest event repaired.
// Detection uses the residual of a short-term linear predictor; repair is least-squares
// autoregressive interpolation from the surrounding audio (both channels detected jointly).
ClickStats declick(Channels& ch, double rate, double sensitivity, double maxClickMs);
// Crackle: many tiny impulses. Several low-threshold, short-gap passes.
ClickStats decrackle(Channels& ch, double rate, double amount);

// ---- Manual repair of a marked stretch -----------------------------------------------
// All three work on a window of audio that includes context either side of the gap [g0, g1)
// (at least a few thousand samples where possible) and change only the gap and, for the last
// two, short blends at its edges. They return false (audio untouched) when they can't work.
//
// Interpolation: rebuilds the gap from an autoregressive model of the audio on both sides (the
// least-squares solution, so it joins smoothly at both ends). Excellent up to about 20 ms; it
// loses level in the middle of much longer gaps. At most kMaxInterpolate samples.
constexpr int64_t kMaxInterpolate = 24000;
bool interpolateGap(Channels& ch, int64_t g0, int64_t g1);
// Replacement: finds the stretch nearby (within the window) whose surroundings best match the
// gap's surroundings, and blends it in. For longer damage in repetitive material. `score`
// (0..1) says how good the match was; below 0.35 nothing is changed.
bool patchFromSimilar(Channels& ch, double rate, int64_t g0, int64_t g1, double* score = nullptr);
// Thump: rebuilds only the low frequencies (below `cutoffHz`) across the gap and keeps
// everything above as it was. For plosives, mic bumps and other low-frequency pops.
bool dethump(Channels& ch, double rate, int64_t g0, int64_t g1, double cutoffHz = 150.0);

// ---- Finding damage ----------------------------------------------------------------------
struct ClickEvent {
    int64_t start = 0, length = 0;   // samples
    double strength = 0;             // how far above the surrounding level (1 = at the threshold)
};
// The clicks declick() would repair at this sensitivity, without changing anything.
std::vector<ClickEvent> findClicks(const Channels& ch, double rate, double sensitivity, double maxClickMs);
// Repairs exactly these (by interpolation). Returns how many were repaired.
int repairClicks(Channels& ch, const std::vector<ClickEvent>& events);

// Digital faults in a recording: a buffer of old audio played again (the same samples as `lag`
// earlier, bit for bit), or a plain break in the waveform (lag 0). The first is certain; the
// second can be a real transient, so it is only reported when it is far out of character.
struct Glitch {
    int64_t start = 0, length = 0;
    int64_t lag = 0;      // > 0: repeated audio from this many samples earlier
    double jump = 0;      // size of the step into it (linear)
};
// Scans win[from, to); samples before `from` are history (up to 2^18 are used to recognise
// repeats). Positions are reported with `base` added. Appends to `out`.
void scanGlitches(const Channels& win, size_t from, size_t to, int64_t base, std::vector<Glitch>& out);

// ---- Broadband noise --------------------------------------------------------------
struct NoiseProfile {
    int fftSize = 0;
    double rate = 0;
    std::vector<double> power;   // mean power per bin (fftSize/2 + 1)
    bool valid() const { return !power.empty(); }
};
NoiseProfile learnNoise(const Channels& noiseOnly, double rate);
// reductionDb: maximum attenuation of noise-dominated bins (e.g. 6..24).
// sensitivity: 0.5 (gentle) .. 3 (aggressive) multiplier on the noise estimate.
// Gains are computed once from both channels and applied to each, preserving the image.
void denoise(Channels& ch, double rate, const NoiseProfile& np, double reductionDb, double sensitivity);

// ---- Spectral editing ------------------------------------------------------------------
enum class SpectralMode { Attenuate, Heal };
// Edits the time-frequency rectangle [t0,t1) x [f0,f1). Attenuate: scale by gainDb.
// Heal: replace the magnitudes inside with an interpolation of the frames just before and
// after (removes coughs, chair squeaks, phone rings), keeping a smooth transition.
void spectralEdit(Channels& ch, double rate, double t0, double t1, double f0, double f1, SpectralMode mode, double gainDb);

// Spectrogram for display: frames x bins of dB values (mono sum), fftSize chosen from rate.
struct Spectrogram {
    int fftSize = 0, hop = 0;
    double rate = 0;
    std::vector<std::vector<float>> db;   // [frame][bin]
};
Spectrogram spectrogram(const Channels& ch, double rate, int maxFrames = 4000);

int stftSizeFor(double rate);   // 4096 at 44.1/48 kHz, scaled with the rate

}  // namespace lathe::dsp
