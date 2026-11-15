// Tests for RecentSessions. `dir` is an empty scratch folder.
// Meant to be #included into a test program that defines CHECK(cond) and includes JUCE and
// RecentSessions.h.
static void recentSessionTests(const juce::File& dir) {
    using lathe::RecentSessions;
    const juce::String dash = juce::String::charToString(static_cast<juce::juce_wchar>(0x2014));

    const juce::File fa = dir.getChildFile("one.lathe");
    const juce::File fb = dir.getChildFile("two.lathe");
    const juce::File fc = dir.getChildFile("three.lathe");

    // Order after adds: newest first.
    {
        RecentSessions r;
        CHECK(r.size() == 0);
        r.add(fa);
        r.add(fb);
        r.add(fc);
        CHECK(r.size() == 3);
        CHECK(r[0] == fc);
        CHECK(r[1] == fb);
        CHECK(r[2] == fa);

        // Re-adding moves to the top without duplicating.
        r.add(fa);
        CHECK(r.size() == 3);
        CHECK(r[0] == fa);
        CHECK(r[1] == fc);
        CHECK(r[2] == fb);

        // juce::File() is ignored.
        r.add(juce::File());
        CHECK(r.size() == 3);
        CHECK(r[0] == fa);

        // Remove.
        r.remove(fc);
        CHECK(r.size() == 2);
        CHECK(r[0] == fa);
        CHECK(r[1] == fb);
        r.remove(dir.getChildFile("not-in-the-list.lathe"));
        CHECK(r.size() == 2);

        // Clear.
        r.clear();
        CHECK(r.size() == 0);
        CHECK(r[0] == juce::File());
    }

    // Cap at kMax: add 12, keep the newest 10, the oldest two are gone.
    {
        RecentSessions r;
        for (int i = 0; i < 12; ++i) r.add(dir.getChildFile("cap" + juce::String(i) + ".lathe"));
        CHECK(r.size() == RecentSessions::kMax);
        CHECK(r[0] == dir.getChildFile("cap11.lathe"));
        CHECK(r[RecentSessions::kMax - 1] == dir.getChildFile("cap2.lathe"));
        bool oldGone = true;
        for (int i = 0; i < r.size(); ++i)
            if (r[i] == dir.getChildFile("cap0.lathe") || r[i] == dir.getChildFile("cap1.lathe")) oldGone = false;
        CHECK(oldGone);
    }

    // Out of range.
    {
        RecentSessions r;
        r.add(fa);
        CHECK(r[-1] == juce::File());
        CHECK(r[1] == juce::File());
        CHECK(r[100] == juce::File());
        CHECK(!r.exists(-1));
        CHECK(!r.exists(5));
    }

    // toString / fromString round trip.
    {
        RecentSessions r;
        r.add(fa);
        r.add(fb);
        r.add(fc);
        const juce::String s = r.toString();
        RecentSessions r2;
        r2.fromString(s);
        CHECK(r2.size() == 3);
        CHECK(r2[0] == fc);
        CHECK(r2[1] == fb);
        CHECK(r2[2] == fa);
        CHECK(r2.toString() == s);

        // Blank lines and duplicate lines in the input are ignored; order is kept.
        const juce::String messy = "\n" + fa.getFullPathName() + "\n\n" + fb.getFullPathName() + "\n" +
                                   fa.getFullPathName() + "\n   \n" + fc.getFullPathName() + "\n";
        RecentSessions r3;
        r3.fromString(messy);
        CHECK(r3.size() == 3);
        CHECK(r3[0] == fa);
        CHECK(r3[1] == fb);
        CHECK(r3[2] == fc);

        // fromString replaces what was there, and an empty string empties the list.
        r3.fromString(juce::String());
        CHECK(r3.size() == 0);

        // More than kMax lines: keep the first kMax.
        juce::String many;
        for (int i = 0; i < 15; ++i) many += dir.getChildFile("m" + juce::String(i) + ".lathe").getFullPathName() + "\n";
        RecentSessions r4;
        r4.fromString(many);
        CHECK(r4.size() == RecentSessions::kMax);
        CHECK(r4[0] == dir.getChildFile("m0.lathe"));
        CHECK(r4[RecentSessions::kMax - 1] == dir.getChildFile("m9.lathe"));
    }

    // exists(): true for a real file, false for a missing one.
    {
        const juce::File real = dir.getChildFile("real.lathe");
        CHECK(real.replaceWithText("x"));
        const juce::File missing = dir.getChildFile("missing.lathe");
        RecentSessions r;
        r.add(missing);
        r.add(real);
        CHECK(r.exists(0));
        CHECK(!r.exists(1));
    }

    // label(): a normal entry.
    {
        const juce::File solo = dir.getChildFile("Solo").getChildFile("Song.lathe");
        RecentSessions r;
        r.add(solo);
        CHECK(r.label(0) == "Song  " + dash + "  Solo");
        CHECK(r.label(5) == juce::String());
    }

    // label(): same name and same parent folder name in different places use the full parent path.
    {
        const juce::File one = dir.getChildFile("a").getChildFile("Mix").getChildFile("Album.lathe");
        const juce::File two = dir.getChildFile("b").getChildFile("Mix").getChildFile("Album.lathe");
        CHECK(one.getParentDirectory().createDirectory().wasOk());
        CHECK(two.getParentDirectory().createDirectory().wasOk());
        RecentSessions r;
        r.add(one);
        r.add(two);
        CHECK(r.size() == 2);
        CHECK(r.label(0) != r.label(1));
        CHECK(r.label(0).contains(two.getParentDirectory().getFullPathName()));
        CHECK(r.label(1).contains(one.getParentDirectory().getFullPathName()));

        // A third entry with a different name is unaffected.
        r.add(dir.getChildFile("Mix").getChildFile("Other.lathe"));
        CHECK(r.label(0) == "Other  " + dash + "  Mix");
    }
}
