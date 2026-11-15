#include "RepairPanel.h"

namespace lathe {

namespace {
constexpr int kRowH = 22, kTickZone = 26, kTimeW = 90, kLengthW = 110;
const juce::Colour kCertain{0xffe0504a}, kMaybe{0xfff0c43a};
}  // namespace

void RepairPanel::setupLabel(juce::Label& l, const juce::String& text, bool dim, float size, bool bold) {
    l.setText(text, juce::dontSendNotification);
    l.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour(juce::Label::textColourId, dim ? colours::dim : colours::text);
    l.setMinimumHorizontalScale(1.0f);
    l.setBorderSize({0, 0, 0, 0});
}

RepairPanel::RepairPanel() {
    setupLabel(header_, "Select a region on the timeline", false, 15.0f, true);
    setupLabel(heading1_, "REPAIR THE IN-OUT", true, 11.0f, true);
    setupLabel(help1_, "Mark the damage with In (I) and Out (O), as tightly as you can, then choose how to rebuild it.", true);
    setupLabel(heading2_, "FIND CLICKS AND FAULTS", true, 11.0f, true);
    setupLabel(help2_, "Looks in the In-Out if one is marked, otherwise the whole region. Nothing is changed until you press Repair Ticked.", true);
    setupLabel(heading3_, "CHECK YOUR WORK", true, 11.0f, true);
    setupLabel(help3_, "Play as usual while switching. What was removed should sound like the fault alone, with no music in it.", true);
    setupLabel(sensLabel_, "Sensitivity", true);
    setupLabel(lenLabel_, "Longest click", true);
    setupLabel(count_, "0 of 0 ticked", true);
    setupLabel(empty_, "Nothing found yet. Press Find Clicks, or Check for Recording Faults.", true, 13.0f);
    setupLabel(status_, {}, false, 12.5f);
    empty_.setJustificationType(juce::Justification::centred);
    empty_.setInterceptsMouseClicks(false, false);
    for (auto* l : {&help1_, &help2_, &help3_, &status_}) l->setJustificationType(juce::Justification::topLeft);

    mode_.addItem("Interpolate (clicks, dropouts, up to about a quarter second)", 1);
    mode_.addItem("Replace from similar audio nearby (longer gaps in steady material)", 2);
    mode_.addItem("De-pop (remove a low thump, keep the music on top)", 3);
    mode_.setSelectedId(1, juce::dontSendNotification);

    sens_.setSliderStyle(juce::Slider::LinearHorizontal);
    sens_.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    sens_.setRange(0.0, 1.0, 0.01);
    sens_.setValue(0.5, juce::dontSendNotification);
    len_.setSliderStyle(juce::Slider::LinearHorizontal);
    len_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    len_.setRange(0.2, 5.0, 0.1);
    len_.setValue(2.0, juce::dontSendNotification);
    len_.setTextValueSuffix(" ms");

    list_.setRowHeight(kRowH);
    list_.setColour(juce::ListBox::backgroundColourId, colours::panel);
    list_.setColour(juce::ListBox::outlineColourId, colours::line);
    list_.setOutlineThickness(1);

    for (auto* b : {&repairInOut_, &removeInOut_, &find_, &check_, &tickAll_, &untickAll_, &clear_, &repairTicked_})
        styleButton(*b, b == &repairInOut_ || b == &repairTicked_);
    for (int i = 0; i < 3; ++i) {
        auto& b = hear_[i];
        styleButton(b);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(71);
        b.onClick = [this, i] {
            hearMode_ = i;
            if (onHearMode) onHearMode(i);
        };
    }
    hear_[0].setToggleState(true, juce::dontSendNotification);

    repairInOut_.onClick = [this] { if (onRepairInOut) onRepairInOut(); };
    removeInOut_.onClick = [this] { if (onRemoveInOut) onRemoveInOut(); };
    find_.onClick = [this] { if (onFindClicks) onFindClicks(); };
    check_.onClick = [this] { if (onCheckFaults) onCheckFaults(); };
    repairTicked_.onClick = [this] { if (onRepairTicked) onRepairTicked(); };
    clear_.onClick = [this] { if (onClearList) onClearList(); };
    tickAll_.onClick = [this] { setAllTicks(true); };
    untickAll_.onClick = [this] { setAllTicks(false); };

    for (auto* c : std::initializer_list<juce::Component*>{&header_, &heading1_, &help1_, &heading2_, &help2_, &heading3_, &help3_,
                                                           &sensLabel_, &lenLabel_, &count_, &status_, &mode_, &sens_, &len_, &list_,
                                                           &empty_, &repairInOut_, &removeInOut_, &find_, &check_, &tickAll_,
                                                           &untickAll_, &clear_, &repairTicked_, &hear_[0], &hear_[1], &hear_[2]})
        addAndMakeVisible(*c);
    updateCount();
    setSize(620, 640);
}

void RepairPanel::setRegionName(const juce::String& name) {
    header_.setText(name.isEmpty() ? juce::String("Select a region on the timeline") : "Repairing: " + name, juce::dontSendNotification);
}

void RepairPanel::setRows(const juce::Array<Row>& rows) {
    auto sel = list_.getSelectedRow();
    auto pos = list_.getVerticalPosition();
    rows_ = rows;
    list_.updateContent();
    if (sel >= 0 && sel < rows_.size()) list_.selectRow(sel, true, true);
    else list_.deselectAllRows();
    list_.setVerticalPosition(pos);
    list_.repaint();
    updateCount();
}

void RepairPanel::setStatus(const juce::String& text) {
    warning_ = text.startsWithChar('!');
    status_.setText(warning_ ? text.substring(1).trimStart() : text, juce::dontSendNotification);
    status_.setColour(juce::Label::textColourId, warning_ ? kMaybe : colours::text);
}

void RepairPanel::setHearMode(int m) {
    hearMode_ = juce::jlimit(0, 2, m);
    hear_[hearMode_].setToggleState(true, juce::dontSendNotification);
}

int RepairPanel::hearMode() const { return hearMode_; }
int RepairPanel::mode() const { return juce::jmax(0, mode_.getSelectedId() - 1); }
double RepairPanel::sensitivity() const { return sens_.getValue(); }
double RepairPanel::maxClickMs() const { return len_.getValue(); }
int RepairPanel::selectedRow() const { return list_.getSelectedRow(); }

void RepairPanel::updateCount() {
    int total = 0, ticked = 0;
    for (auto& r : rows_)
        if (!r.repaired) { ++total; if (r.ticked) ++ticked; }
    count_.setText(juce::String(ticked) + " of " + juce::String(total) + " ticked", juce::dontSendNotification);
    empty_.setVisible(rows_.isEmpty());
}

void RepairPanel::setAllTicks(bool value) {
    for (int i = 0; i < rows_.size(); ++i) {
        auto& r = rows_.getReference(i);
        if (r.repaired || r.ticked == value) continue;
        r.ticked = value;
        if (onTick) onTick(i, value);
    }
    list_.repaint();
    updateCount();
}

void RepairPanel::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) {
    if (row < 0 || row >= rows_.size()) return;
    const auto& r = rows_.getReference(row);
    g.fillAll(row % 2 ? colours::panel2 : colours::panel);
    if (selected) g.fillAll(colours::accent.withAlpha(0.25f));
    const float a = r.repaired ? 0.5f : 1.0f;

    if (!r.repaired) {
        auto box = juce::Rectangle<float>(6.0f, (h - 14) * 0.5f, 14.0f, 14.0f);
        if (r.ticked) {
            g.setColour(colours::accent);
            g.fillRoundedRectangle(box, 2.0f);
            juce::Path tick;
            tick.startNewSubPath(box.getX() + 3.0f, box.getCentreY());
            tick.lineTo(box.getX() + 6.0f, box.getBottom() - 4.0f);
            tick.lineTo(box.getRight() - 3.0f, box.getY() + 3.5f);
            g.setColour(colours::bg);
            g.strokePath(tick, juce::PathStrokeType(1.8f));
        } else {
            g.setColour(colours::dim);
            g.drawRoundedRectangle(box.reduced(0.5f), 2.0f, 1.0f);
        }
    }

    int x = kTickZone;
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 12.5f, juce::Font::plain));
    g.setColour(colours::text.withMultipliedAlpha(a));
    g.drawText(r.time, x, 0, kTimeW, h, juce::Justification::centredLeft, true);
    x += kTimeW;

    g.setFont(juce::FontOptions(13.0f));
    g.setColour((r.certain ? kCertain : kMaybe).withMultipliedAlpha(a));
    int lenX = w - kLengthW - 8;
    g.drawText(r.kind, x, 0, juce::jmax(0, lenX - x - 6), h, juce::Justification::centredLeft, true);

    g.setColour(colours::dim.withMultipliedAlpha(a));
    if (r.repaired) {
        g.setFont(juce::FontOptions(12.0f, juce::Font::italic));
        g.drawText("repaired", lenX, 0, kLengthW, h, juce::Justification::centredRight, true);
    } else {
        g.drawText(r.length, lenX, 0, kLengthW, h, juce::Justification::centredRight, true);
    }
}

void RepairPanel::listBoxItemClicked(int row, const juce::MouseEvent& e) {
    if (row < 0 || row >= rows_.size()) return;
    auto& r = rows_.getReference(row);
    if (e.x < kTickZone && !r.repaired) {
        r.ticked = !r.ticked;
        list_.repaintRow(row);
        updateCount();
        if (onTick) onTick(row, r.ticked);
    } else if (onRowClicked) {
        onRowClicked(row);
    }
}

void RepairPanel::paint(juce::Graphics& g) {
    g.fillAll(colours::bg);
    g.setColour(colours::line);
    for (auto* l : {&heading1_, &heading2_, &heading3_}) g.drawHorizontalLine(l->getY() - 5, 12.0f, float(getWidth() - 12));
}

void RepairPanel::resized() {
    auto r = getLocalBounds().reduced(12);
    const int gap = 6;
    auto take = [&](int h) { auto x = r.removeFromTop(h); r.removeFromTop(gap); return x; };
    auto buttonsRow = [](juce::Rectangle<int> row, std::initializer_list<std::pair<juce::Component*, int>> items) {
        for (auto& [c, w] : items) { c->setBounds(row.removeFromLeft(w)); row.removeFromLeft(6); }
    };

    // Status sits at the bottom (two lines).
    status_.setBounds(r.removeFromBottom(34));
    r.removeFromBottom(gap);

    header_.setBounds(take(22));
    r.removeFromTop(4);
    heading1_.setBounds(take(14));
    help1_.setBounds(take(32));
    auto row = take(28);
    repairInOut_.setBounds(row.removeFromRight(110));
    row.removeFromRight(6);
    removeInOut_.setBounds(row.removeFromRight(170));
    row.removeFromRight(6);
    mode_.setBounds(row);

    r.removeFromTop(6);
    heading2_.setBounds(take(14));
    row = take(24);
    sensLabel_.setBounds(row.removeFromLeft(72));
    sens_.setBounds(row.removeFromLeft(juce::jmax(60, (row.getWidth() - 100) / 2)));
    row.removeFromLeft(10);
    lenLabel_.setBounds(row.removeFromLeft(86));
    len_.setBounds(row);
    row = take(28);
    buttonsRow(row, {{&find_, 110}, {&check_, 200}});
    help2_.setBounds(take(32));

    // Fixed-height blocks below the list.
    const int below = 28 + gap + 14 + gap + 28 + gap + 32 + gap + 6;
    auto belowArea = r.removeFromBottom(below);
    list_.setBounds(r.removeFromTop(juce::jmax(60, r.getHeight() - gap)));
    empty_.setBounds(list_.getBounds().reduced(10, 0));

    belowArea.removeFromTop(gap);
    row = belowArea.removeFromTop(28);
    belowArea.removeFromTop(gap);
    repairTicked_.setBounds(row.removeFromRight(110));
    row.removeFromRight(6);
    clear_.setBounds(row.removeFromRight(80));
    row.removeFromRight(10);
    buttonsRow(row, {{&tickAll_, 70}, {&untickAll_, 80}});
    count_.setBounds(row);

    belowArea.removeFromTop(6);
    heading3_.setBounds(belowArea.removeFromTop(14));
    belowArea.removeFromTop(gap);
    row = belowArea.removeFromTop(28);
    belowArea.removeFromTop(gap);
    const int w = (row.getWidth() - 12) / 3;
    for (int i = 0; i < 3; ++i) {
        hear_[i].setBounds(row.removeFromLeft(w));
        row.removeFromLeft(6);
    }
    help3_.setBounds(belowArea);
}

}  // namespace lathe
