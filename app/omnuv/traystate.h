// Omnuv: what the tray icon claims, decided once from readings rather than
// from whichever signal happened to arrive last.
//
// **No Qt in this file, deliberately.** D1's exit evidence is the widget
// plan's own check — *feed every local probe a failure and assert no row is
// green* — and a check that needs a Qt build, a window and a notification area
// is a check nobody runs. So everything the icon decides is a pure function of
// enumerated readings, and `.github/workflows/omnuv-change-budget.yml`
// compiles this header with a bare `g++` and asserts on it on every push,
// off the rig and in a second.
//
// That is also why there is no QString here and no QObject: `tray.cpp` fills a
// `Readings` in from the live objects and renders the answer. Nothing in this
// file knows what a menu or a pixel is.

#pragma once

namespace omnuv {

// `omnuv_protocol::CheckResult`'s three, and the third one is the whole point:
// "the difference between 'broken' and 'not known' is most of the value of
// checking at all". A reading that could not be taken is never a pass, and
// never a failure either.
enum class Reading { Pass, Fail, Unknown };

// Everything the icon is allowed to look at. Every field is a reading somebody
// actually took; there is no field here that means "probably".
struct Readings
{
    // Nobody has asked this device to watch anything. Neither of these is a
    // fault: "an action the person has not taken yet is not a failure".
    bool signedIn = false;
    bool tunnelInstalled = false;

    // In flight, and it resolves by itself without anybody doing anything.
    bool joining = false;
    int machinesMoving = 0;

    // We looked, and it is wrong.
    int machinesUnhappy = 0;

    // The three rows of *This device*, each probed by this machine's own
    // network stack.
    Reading internet = Reading::Unknown;
    Reading omnuv = Reading::Unknown;
    Reading network = Reading::Unknown;
};

// Four, and each one implies a different thing to do.
//
//   Connected   nothing to do
//   Working     wait; it resolves on its own
//   Attention   look; it does not
//   Idle        nothing is being claimed
//
// **There is deliberately no fifth for "cannot tell".** The rule that unknown
// is a first-class state is about *rows*, where there is room to render it as
// its own thing with its own word, and the menu does exactly that. An icon has
// four pixels of information, and a fifth glyph that a person cannot tell from
// the fourth at 16px is worse than four — it costs the distinction between the
// three that were already distinguishable. So `Idle` is named for what it
// claims rather than for why: *this icon is asserting nothing right now*,
// which covers "nobody asked us to watch", "the network is down" and "we could
// not look" alike. The menu tells those apart in words, where there is room.
enum class TrayIcon { Idle, Working, Attention, Connected };

constexpr TrayIcon iconFor(const Readings& r)
{
    // 1. Nobody asked us to watch anything. Signing in and installing the
    //    network client are both actions the person has not taken, not faults,
    //    and an icon that shows trouble for them teaches people to ignore
    //    trouble.
    if (!r.signedIn || !r.tunnelInstalled) {
        return TrayIcon::Idle;
    }

    // 2. We looked, and something is wrong that nobody already knows about.
    //    Above `Working`, because a failure somebody must act on outranks a
    //    wait that ends by itself.
    //
    //    **The `internet` term is what stops this being noise**, and it is the
    //    one thing that row is for. Omnuv not answering a laptop on a train is
    //    a fact Windows is already showing in the same taskbar, four icons
    //    along; repeating it accents our icon for a condition the person
    //    cannot act on and did not need telling. So an Omnuv failure raises
    //    attention only while it is *unexplained* — the connection works and
    //    Omnuv still did not answer, which is the case worth a glance.
    if (r.machinesUnhappy > 0 || (r.omnuv == Reading::Fail && r.internet != Reading::Fail)) {
        return TrayIcon::Attention;
    }

    // 3. Something is in flight and resolves on its own.
    if (r.joining || r.machinesMoving > 0) {
        return TrayIcon::Working;
    }

    // 4. Connected is a claim, so it needs positive evidence on both rows
    //    rather than the absence of bad news. A reading that could not be
    //    taken lands on 5, not here: a green dot over a probe that never
    //    returned is precisely the 10 September failure.
    if (r.network == Reading::Pass && r.omnuv == Reading::Pass) {
        return TrayIcon::Connected;
    }

    // 5. Watching, and asserting nothing: the network is down, or a probe
    //    could not be taken.
    //
    //    **A dropped tunnel lands here rather than on attention, deliberately,
    //    and it is the closest call in this function.** The client cannot tell
    //    a device that dropped off the network from one that has simply never
    //    joined — `join()` learns that, `check()` does not — so treating the
    //    reading as a fault would accent the icon permanently for somebody who
    //    has merely not pressed Join yet. That is the antipattern the widget
    //    plan names outright: "a dashboard that shows red for 'you have not
    //    paired this rig' teaches people to ignore red." Idle claims nothing,
    //    which is true in both cases, and the menu's own row carries the word
    //    and the sentence that tell them apart.
    return TrayIcon::Idle;
}

} // namespace omnuv
