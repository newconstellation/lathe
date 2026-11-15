#include "Repair.h"

namespace lathe {

PatchSet::PatchSet(juce::AudioFormatManager& fm, const Region& r, juce::int64 from, juce::int64 to) {
    juce::String openPath;
    std::unique_ptr<juce::AudioFormatReader> reader;
    for (auto& p : r.patches) {
        if (p.length <= 0 || p.pos + p.length <= from || p.pos >= to) continue;
        if (p.file != openPath) {
            reader.reset(fm.createReaderFor(juce::File(p.file)));
            openPath = p.file;
        }
        if (!reader || p.offset < 0 || p.offset + p.length > reader->lengthInSamples) continue;   // (a missing patch file: the original plays)
        Item it;
        it.pos = p.pos;
        it.length = p.length;
        juce::AudioBuffer<float> buf(2, p.length);
        reader->read(&buf, 0, p.length, p.offset, true, true);
        if (reader->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, p.length);
        it.l.assign(buf.getReadPointer(0), buf.getReadPointer(0) + p.length);
        it.r.assign(buf.getReadPointer(1), buf.getReadPointer(1) + p.length);
        items_.push_back(std::move(it));
    }
}

void PatchSet::apply(float* const* data, int channels, juce::int64 pos, int n, int mode) const {
    if (mode == 1 || channels <= 0) return;
    if (mode == 2) {
        // What the repairs removed: the original minus the repaired audio, nothing elsewhere.
        std::vector<float> orig[2];
        const int nc = juce::jmin(2, channels);
        for (int c = 0; c < nc; ++c) orig[c].assign(data[c], data[c] + n);
        apply(data, channels, pos, n, 0);
        std::vector<char> covered(size_t(n), 0);
        for (auto& it : items_) {
            const juce::int64 a = juce::jmax(pos, it.pos), b = juce::jmin(pos + n, it.pos + it.length);
            for (juce::int64 k = a; k < b; ++k) covered[size_t(k - pos)] = 1;
        }
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i) data[c][i] = c < nc && covered[size_t(i)] ? orig[c][size_t(i)] - data[c][i] : 0.0f;
        return;
    }
    for (auto& it : items_) {
        const juce::int64 a = juce::jmax(pos, it.pos), b = juce::jmin(pos + n, it.pos + it.length);
        for (juce::int64 k = a; k < b; ++k) {
            data[0][k - pos] = it.l[size_t(k - it.pos)];
            if (channels > 1) data[1][k - pos] = it.r[size_t(k - it.pos)];
        }
    }
}

bool readPatched(juce::AudioFormatManager& fm, const Region& r, juce::int64 pos, juce::int64 n, dsp::Channels& out) {
    std::unique_ptr<juce::AudioFormatReader> rd(fm.createReaderFor(r.file));
    if (!rd || n <= 0) return false;
    out.assign(2, std::vector<double>(size_t(n), 0.0));
    PatchSet patches(fm, r, pos, pos + n);
    const int block = 65536;
    juce::AudioBuffer<float> buf(2, block);
    for (juce::int64 p = 0; p < n; p += block) {
        const int k = int(juce::jmin<juce::int64>(block, n - p));
        buf.clear();
        rd->read(&buf, 0, k, pos + p, true, true);   // (zeros outside the file)
        if (rd->numChannels == 1) buf.copyFrom(1, 0, buf, 0, 0, k);
        patches.apply(buf.getArrayOfWritePointers(), 2, pos + p, k, 0);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < k; ++i) out[size_t(c)][size_t(p + i)] = buf.getSample(c, i);
    }
    return true;
}

namespace {
// A patch file being written: every patch is appended to it.
struct PatchWriter {
    PatchWriter(const juce::File& folder, const Region& r, double rate) {
        folder.createDirectory();
        const auto base = juce::File::createLegalFileName(r.name.upToFirstOccurrenceOf(" (", false, false) + " repair");
        file = folder.getNonexistentChildFile(base, ".wav", false);
        std::unique_ptr<juce::OutputStream> os(file.createOutputStream().release());
        if (!os) return;
        juce::WavAudioFormat wav;
        writer.reset(wav.createWriterFor(os.get(), rate, 2, 32, {}, 0));
        if (writer) os.release();
    }
    // The changed samples of `work` against `orig` around [a, b) (window-local) become a patch.
    bool add(const dsp::Channels& orig, const dsp::Channels& work, juce::int64 windowPos, juce::Array<Patch>& out) {
        if (!writer) return false;
        const juce::int64 n = juce::int64(orig[0].size());
        juce::int64 first = -1, last = -1;
        for (juce::int64 i = 0; i < n; ++i)
            if (orig[0][size_t(i)] != work[0][size_t(i)] || orig[1][size_t(i)] != work[1][size_t(i)]) {
                if (first < 0) first = i;
                last = i;
            }
        if (first < 0) return false;
        const int len = int(last - first + 1);
        juce::AudioBuffer<float> b(2, len);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < len; ++i) b.setSample(c, i, float(work[size_t(c)][size_t(first + i)]));
        if (!writer->writeFromAudioSampleBuffer(b, 0, len)) return false;
        out.add({windowPos + first, len, file.getFullPathName(), written});
        written += len;
        return true;
    }
    void close() {
        writer.reset();
        if (written == 0) file.deleteFile();
    }
    juce::File file;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 written = 0;
};

juce::String ms(juce::int64 samples, double rate) { return juce::String(double(samples) * 1000.0 / rate, samples * 1000.0 / rate < 10 ? 2 : 1) + " ms"; }
}  // namespace

RepairResult repairRange(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b, RepairMode mode, const juce::File& folder) {
    RepairResult res;
    const double rate = r.fileRate;
    a = juce::jmax<juce::int64>(a, 0);
    b = juce::jmin(b, r.fileLength);
    if (b <= a) { res.error = "The In-Out doesn't cover any of this region."; return res; }
    const juce::int64 len = b - a;
    if (mode == RepairMode::Interpolate && len > dsp::kMaxInterpolate) {
        res.error = "Interpolation rebuilds up to " + ms(dsp::kMaxInterpolate, rate) + " at this sample rate; the In-Out is " + ms(len, rate) +
                    ". Mark less, or use Replace from similar audio.";
        return res;
    }
    if (len > juce::int64(rate * 2.0)) { res.error = "Mark a shorter stretch (2 seconds at most)."; return res; }
    // Context either side: plenty for the model; a few seconds to search for similar audio.
    const juce::int64 ctx = mode == RepairMode::Similar ? juce::int64(rate * 4.0) : juce::jmax<juce::int64>(juce::int64(rate * 0.5), len * 8);
    const juce::int64 w0 = a - ctx;
    dsp::Channels orig;
    if (!readPatched(fm, r, w0, len + 2 * ctx, orig)) { res.error = "Couldn't read " + r.file.getFullPathName(); return res; }
    dsp::Channels work = orig;
    bool done = false;
    if (mode == RepairMode::Interpolate) {
        done = dsp::interpolateGap(work, ctx, ctx + len);
        if (!done) res.error = "Couldn't rebuild that stretch (not enough audio either side of it).";
        else res.report = "Rebuilt " + ms(len, rate) + " (" + juce::String(len) + " samples) from the audio either side.";
    } else if (mode == RepairMode::Similar) {
        double score = 0;
        done = dsp::patchFromSimilar(work, rate, ctx, ctx + len, &score);
        if (!done) res.error = "Nothing nearby is similar enough to stand in for it (best match " + juce::String(juce::roundToInt(score * 100)) + " %).";
        else res.report = "Replaced " + ms(len, rate) + " with matching audio from nearby (match " + juce::String(juce::roundToInt(score * 100)) + " %).";
    } else {
        done = dsp::dethump(work, rate, ctx, ctx + len);
        if (!done) res.error = "Couldn't rebuild the low frequencies there (not enough audio either side).";
        else res.report = "Rebuilt the low frequencies (below 150 Hz) across " + ms(len, rate) + "; everything above is untouched.";
    }
    if (!done) return res;
    PatchWriter pw(folder, r, rate);
    if (!pw.add(orig, work, w0, res.patches)) {
        pw.close();
        res.error = pw.writer || pw.file.existsAsFile() ? "The repair changed nothing (that stretch was already what the audio around it predicts)." : "Couldn't write the repair in " + folder.getFullPathName();
        return res;
    }
    pw.close();
    res.ok = true;
    return res;
}

RepairResult repairSpots(juce::AudioFormatManager& fm, const Region& r, const juce::Array<RepairSpot>& spots, const juce::File& folder,
                         const std::function<bool(double)>& progress) {
    RepairResult res;
    if (spots.isEmpty()) { res.error = "Nothing is ticked."; return res; }
    const double rate = r.fileRate;
    PatchWriter pw(folder, r, rate);
    if (!pw.writer) { res.error = "Couldn't write the repair in " + folder.getFullPathName(); return res; }
    Region cur = r;   // (repairs made here are seen by the next ones)
    int fixed = 0, skipped = 0;
    for (int i = 0; i < spots.size(); ++i) {
        const auto& sp = spots.getReference(i);
        const juce::int64 len = juce::jmin<juce::int64>(sp.length, dsp::kMaxInterpolate);
        const juce::int64 ctx = juce::jmax<juce::int64>(juce::int64(rate * 0.25), len * 8);
        dsp::Channels orig;
        if (len <= 0 || !readPatched(fm, cur, sp.pos - ctx, len + 2 * ctx, orig)) { ++skipped; continue; }
        dsp::Channels work = orig;
        juce::Array<Patch> made;
        if (dsp::interpolateGap(work, ctx, ctx + len) && pw.add(orig, work, sp.pos - ctx, made)) {
            res.patches.addArray(made);
            cur.patches.addArray(made);
            ++fixed;
        } else {
            ++skipped;
        }
        if (progress && !progress(double(i + 1) / spots.size())) { pw.close(); res.error = "Cancelled."; res.patches.clear(); return res; }
    }
    pw.close();
    if (fixed == 0) { res.error = "None of those could be repaired."; return res; }
    res.report = juce::String(fixed) + (fixed == 1 ? " spot repaired" : " spots repaired") + (skipped ? ", " + juce::String(skipped) + " left as they were" : juce::String()) + ".";
    res.ok = true;
    return res;
}

namespace {
// Walks source samples [a, b) in pieces with `history` samples before each, calling fn(window, from, to, base).
template <typename Fn>
bool walk(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b, juce::int64 history, const std::function<bool(double)>& progress, Fn&& fn) {
    const juce::int64 piece = juce::int64(1) << 20;
    for (juce::int64 p = a; p < b; p += piece) {
        const juce::int64 q = juce::jmin(b, p + piece), h = juce::jmin(history, p), tail = juce::jmin<juce::int64>(4096, r.fileLength - q > 0 ? r.fileLength - q : 0);
        dsp::Channels win;
        if (!readPatched(fm, r, p - h, (q - p) + h + tail, win)) return false;
        fn(win, size_t(h), size_t(h + (q - p)), p - h);
        if (progress && !progress(double(q - a) / double(b - a))) return false;
    }
    return true;
}
}  // namespace

juce::Array<RepairSpot> findClicks(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b, double sensitivity, double maxClickMs,
                                   const std::function<bool(double)>& progress) {
    juce::Array<RepairSpot> out;
    a = juce::jmax<juce::int64>(0, a);
    b = juce::jmin(b, r.fileLength);
    walk(fm, r, a, b, 8192, progress, [&](const dsp::Channels& win, size_t from, size_t to, juce::int64 base) {
        for (auto& e : dsp::findClicks(win, r.fileRate, sensitivity, maxClickMs)) {
            if (e.start < int64_t(from) || e.start >= int64_t(to)) continue;   // (the history and tail belong to the neighbouring pieces)
            out.add({base + e.start, e.length, "click", e.strength, false});
        }
    });
    return out;
}

juce::Array<RepairSpot> scanTake(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b,
                                 const std::function<bool(double)>& progress) {
    juce::Array<RepairSpot> out;
    a = juce::jmax<juce::int64>(0, a);
    b = juce::jmin(b, r.fileLength);
    walk(fm, r, a, b, juce::int64(1) << 18, progress, [&](const dsp::Channels& win, size_t from, size_t to, juce::int64 base) {
        std::vector<dsp::Glitch> g;
        dsp::scanGlitches(win, from, to, base, g);
        for (auto& e : g) {
            if (!out.isEmpty() && e.start < out.getLast().pos + out.getLast().length + 8) continue;   // (seen at the end of the piece before)
            if (e.lag > 0) out.add({e.start, e.length, "repeated audio (" + juce::String(e.lag) + " samples back)", e.jump, true});
            else out.add({e.start, e.length, "break", e.jump, false});
        }
    });
    return out;
}

int removePatches(Region& r, juce::int64 a, juce::int64 b) {
    int n = 0;
    for (int i = r.patches.size(); --i >= 0;) {
        const auto& p = r.patches.getReference(i);
        if (p.pos < b && p.pos + p.length > a) { r.patches.remove(i); ++n; }
    }
    return n;
}

}  // namespace lathe
