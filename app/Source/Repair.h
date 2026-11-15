// Repairs: small stretches of a region's audio replaced as it plays (Region::patches). The
// source file is never changed; a repair is a few samples in a patch file next to the session.
//
//  PatchSet        the patch audio of one region, loaded once, applied to blocks read from its file
//  repairRange     rebuilds one marked stretch (interpolate / replace from similar audio / thump)
//  findClicks      lists the clicks in a region or part of it, without changing anything
//  repairSpots     repairs a list of spots by interpolation, as one patch file
//  scanTake        looks through a recording for digital faults (repeated audio, breaks)
#pragma once

#include "Session.h"

#include "../../core/dsp/Restoration.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace lathe {

class PatchSet {
public:
    // Loads the patches of `r` that touch source samples [from, to).
    PatchSet(juce::AudioFormatManager& fm, const Region& r, juce::int64 from, juce::int64 to);
    bool empty() const { return items_.empty(); }
    // `data` holds `n` samples per channel read from the region's file at `pos`.
    // mode 0: the patches replace what they cover. mode 2: everything becomes what the patches
    // removed (original minus patched inside them, silence elsewhere). mode 1 is "do nothing".
    void apply(float* const* data, int channels, juce::int64 pos, int n, int mode) const;

private:
    struct Item { juce::int64 pos; int length; std::vector<float> l, r; };
    std::vector<Item> items_;
};

// Reads [pos, pos + n) of a region's source as two channels (mono doubled), with its repairs
// applied (so new repairs build on earlier ones). Samples outside the file are silence.
bool readPatched(juce::AudioFormatManager& fm, const Region& r, juce::int64 pos, juce::int64 n, dsp::Channels& out);

enum class RepairMode { Interpolate = 0, Similar, Thump };

struct RepairResult {
    bool ok = false;
    juce::String error, report;
    juce::Array<Patch> patches;
};

// A spot to repair, in the region's source-file samples.
struct RepairSpot {
    juce::int64 pos = 0, length = 0;
    juce::String kind;      // "click", "repeated audio (65536 samples back)", "break"
    double strength = 0;
    bool certain = false;   // a digital fault, not a judgement
};

// Rebuilds source samples [a, b) of the region. The patch file is written into `folder`.
RepairResult repairRange(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b, RepairMode mode, const juce::File& folder);
// Interpolates every spot; one patch file for all of them.
RepairResult repairSpots(juce::AudioFormatManager& fm, const Region& r, const juce::Array<RepairSpot>& spots, const juce::File& folder,
                         const std::function<bool(double)>& progress);
// Clicks in source samples [a, b) of the region (as it plays now, repairs included).
juce::Array<RepairSpot> findClicks(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b, double sensitivity, double maxClickMs,
                                   const std::function<bool(double)>& progress);
// Digital faults in source samples [a, b) of the region: repeated audio (certain) and breaks.
juce::Array<RepairSpot> scanTake(juce::AudioFormatManager& fm, const Region& r, juce::int64 a, juce::int64 b,
                                 const std::function<bool(double)>& progress);

// Takes the repairs that touch source samples [a, b) off the region. Returns how many.
int removePatches(Region& r, juce::int64 a, juce::int64 b);

}  // namespace lathe
