// Repair panel: controls and a list of found faults. It holds no repair logic; it shows
// state and calls the hooks below. The owner wires the hooks and feeds rows back in.
#pragma once

#include "Theme.h"
#include <functional>

namespace lathe {

class RepairPanel : public juce::Component, private juce::ListBoxModel {
public:
    struct Row {
        juce::String time;      // already formatted, e.g. "03:12:41"
        juce::String kind;      // "click", "repeated audio (65536 samples back)", "break", "lost audio"
        juce::String length;    // already formatted, e.g. "12 samples" or "1.4 ms"
        bool certain = false;   // a definite digital fault (kind drawn red); otherwise yellow
        bool ticked = true;     // will be repaired by "Repair Ticked"
        bool repaired = false;  // already repaired: drawn dim, "repaired" at the right, no tick box
    };

    RepairPanel();

    void setRegionName(const juce::String& name);   // header text; a prompt when empty
    void setRows(const juce::Array<Row>& rows);     // keeps scroll position and selected row where possible
    const juce::Array<Row>& rows() const { return rows_; }
    void setStatus(const juce::String& text);       // a leading '!' marks a warning
    void setHearMode(int mode);                     // 0 repair, 1 original, 2 removed; does not call onHearMode
    int hearMode() const;
    int mode() const;                               // 0 interpolate, 1 replace from similar audio, 2 de-pop
    double sensitivity() const;                     // 0..1
    double maxClickMs() const;                      // 0.2 .. 5.0
    int selectedRow() const;

    std::function<void()> onRepairInOut, onRemoveInOut, onFindClicks, onCheckFaults, onRepairTicked, onClearList;
    std::function<void(int row)> onRowClicked;         // clicked a row (not its tick box)
    std::function<void(int row, bool ticked)> onTick;  // tick box toggled; rows_ already updated
    std::function<void(int mode)> onHearMode;          // a Hear button was pressed

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    // ListBoxModel
    int getNumRows() override { return rows_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;

    void setAllTicks(bool value);
    void updateCount();
    static void setupLabel(juce::Label& l, const juce::String& text, bool dim, float size = 12.0f, bool bold = false);

    juce::Array<Row> rows_;
    juce::Label header_, heading1_, help1_, heading2_, help2_, heading3_, help3_, sensLabel_, lenLabel_, count_, empty_, status_;
    juce::ComboBox mode_;
    juce::Slider sens_, len_;
    juce::ListBox list_{"faults", this};
    juce::TextButton repairInOut_{"Repair In-Out"}, removeInOut_{"Remove Repairs in In-Out"}, find_{"Find Clicks"},
        check_{"Check for Recording Faults"}, tickAll_{"Tick All"}, untickAll_{"Untick All"}, clear_{"Clear List"},
        repairTicked_{"Repair Ticked"};
    juce::TextButton hear_[3]{juce::TextButton("Hear the Repair"), juce::TextButton("Hear Original"), juce::TextButton("Hear What Was Removed")};
    int hearMode_ = 0;
    bool warning_ = false;
};

}  // namespace lathe
