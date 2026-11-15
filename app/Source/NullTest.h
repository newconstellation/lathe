// Null test / file compare: aligns two renders of the same material (e.g. another editor vs
// Lathe), then measures exactly how they differ. This is the objective way to find out
// what another engine does to the audio: offset, polarity, gain, and what's left over.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

namespace lathe {

struct NullResult {
    bool ok = false;
    juce::String error;
    double rateA = 0, rateB = 0;
    int bitsA = 0, bitsB = 0;
    juce::int64 lengthA = 0, lengthB = 0;
    juce::int64 offset = 0;          // B lags A by this many samples (negative = B leads)
    bool inverted = false;
    juce::int64 compared = 0;        // samples per channel compared
    juce::int64 identical = 0;       // samples (both channels) exactly equal
    double peakA = 0, peakB = 0;     // linear
    double residualRms = 0, residualPeak = 0;      // linear, gain as-is
    double signalRms = 0;
    double gainDiffDb = 0;           // gain that best matches B to A
    double residualRmsGainMatched = 0;

    // Spots where the files differ, in A's sample positions after alignment. Listed only when the
    // files otherwise agree (to within about -100 dBFS); when they differ broadly everywhere,
    // differsThroughout is set and the list stays empty.
    struct Difference {
        juce::int64 start = 0;
        juce::int64 length = 0;
        double peak = 0;             // largest |A - B| in the event, linear
        juce::String kind;           // "repeated audio (N samples back)", "dropped samples (N samples)", "click" or "differs"
        juce::String inFile;         // "A", "B" or "?"
    };
    static constexpr int maxListedDifferences = 200;
    juce::Array<Difference> differences;   // capped at maxListedDifferences
    juce::int64 differenceCount = 0;       // total found (may exceed the cap)
    bool differsThroughout = false;
    double residualPeakOutside = 0;        // largest |A - B| outside the listed events, linear

    juce::String toText(const juce::String& nameA, const juce::String& nameB) const;
};

// Runs on a background thread. `progress` 0..1, return false to cancel.
// If differenceFile is not empty, writes A - B (aligned, gain as-is) as 32-bit float WAV.
NullResult nullTest(juce::AudioFormatManager& fm, const juce::File& a, const juce::File& b, const juce::File& differenceFile,
                    const std::function<bool(double)>& progress);

}  // namespace lathe
