// The File > Open Recent list. Pure data: no windows and no settings I/O here (the caller stores
// toString() in the settings and hands it back through fromString()).
#pragma once

#include <juce_core/juce_core.h>

namespace lathe {

// The File > Open Recent list: the last sessions opened or saved, newest first.
class RecentSessions {
public:
    static constexpr int kMax = 10;

    // Moves the file to the top (compared by full path, case-sensitive) and drops the oldest
    // beyond kMax. A default-constructed juce::File() is ignored.
    void add(const juce::File& f) {
        if (f == juce::File()) return;
        const juce::String p = f.getFullPathName();
        if (p.isEmpty()) return;
        paths_.removeString(p, false);
        paths_.insert(0, p);
        while (paths_.size() > kMax) paths_.remove(paths_.size() - 1);
    }

    void remove(const juce::File& f) { paths_.removeString(f.getFullPathName(), false); }

    void clear() { paths_.clear(); }

    int size() const { return paths_.size(); }

    // juce::File() when out of range.
    juce::File operator[](int i) const {
        if (i < 0 || i >= paths_.size()) return juce::File();
        return juce::File(paths_[i]);
    }

    // Menu label for entry i: the file name without extension, then "  -  " (an em dash) and the
    // parent folder's name; when two entries share both the name and the parent folder's name,
    // the parent's full path is used instead for those entries.
    juce::String label(int i) const {
        if (i < 0 || i >= paths_.size()) return {};
        const juce::File f(paths_[i]);
        const juce::String name = f.getFileNameWithoutExtension();
        const juce::File parent = f.getParentDirectory();
        const juce::String parentName = parent.getFileName();

        bool clash = false;
        for (int j = 0; j < paths_.size() && !clash; ++j) {
            if (j == i) continue;
            const juce::File other(paths_[j]);
            clash = other.getFileNameWithoutExtension() == name &&
                    other.getParentDirectory().getFileName() == parentName;
        }

        const juce::String dash = juce::String::charToString(static_cast<juce::juce_wchar>(0x2014));
        return name + "  " + dash + "  " + (clash ? parent.getFullPathName() : parentName);
    }

    // The file is still there.
    bool exists(int i) const {
        if (i < 0 || i >= paths_.size()) return false;
        return juce::File(paths_[i]).exists();
    }

    // Stored in settings as one string: full paths joined by '\n'.
    juce::String toString() const { return paths_.joinIntoString("\n"); }

    // Ignores blank lines and duplicates, keeps at most kMax, keeps order.
    void fromString(const juce::String& s) {
        paths_.clear();
        const juce::StringArray lines = juce::StringArray::fromLines(s);
        for (const auto& line : lines) {
            if (paths_.size() >= kMax) break;
            const juce::String p = line.trim();
            if (p.isEmpty()) continue;
            paths_.addIfNotAlreadyThere(p, false);
        }
    }

private:
    juce::StringArray paths_;
};

}  // namespace lathe
