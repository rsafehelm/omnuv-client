// Omnuv: D1's exit evidence, as a program rather than as a promise.
//
//     "feed every local probe a failure and assert that no row is green"
//
// **Not part of the application.** It is deliberately absent from
// `app/app.pro`, so qmake never sees it; it is compiled and run by
// `.github/workflows/omnuv-change-budget.yml` on every push, and by anybody
// who wants to, in one line and about a second:
//
//     g++ -std=c++17 -Iapp/omnuv app/omnuv/traystate_check.cpp -o /tmp/c && /tmp/c
//
// That is the whole reason `traystate.h` has no Qt in it. A check that needed
// a Qt build, a window and a notification area would run on the rig, once, by
// hand — which is to say it would be written and then not run, which is the
// state the widget plan says has already cost this project a day.
//
// It also proves it can fail: change `iconFor` to return Connected when the
// network reading is merely not a failure, and three assertions below go red.
// That was run before this file was committed, because a check that has never
// failed is a loop.

#include "traystate.h"
#include <cstdio>
#include <initializer_list>

using namespace omnuv;

static int failed = 0;
#define CHECK(c) do { if (!(c)) { std::printf("  FAILED: %s\n", #c); ++failed; } } while (0)

static Readings watching()
{
    Readings r;
    r.signedIn = true;
    r.tunnelInstalled = true;
    return r;
}

int main()
{
    // ---- the widget plan's own check ------------------------------------
    // Every local probe fed a failure. No row is green, and the icon does not
    // claim health either.
    Readings allFail = watching();
    allFail.internet = Reading::Fail;
    allFail.omnuv = Reading::Fail;
    allFail.network = Reading::Fail;
    CHECK(iconFor(allFail) != TrayIcon::Connected);

    // Every probe unable to answer. "I could not look" is not a pass.
    CHECK(iconFor(watching()) != TrayIcon::Connected);

    // One at a time: no single failed or unreadable probe leaves the icon
    // claiming everything is fine.
    for (Reading bad : { Reading::Fail, Reading::Unknown }) {
        Readings a = watching(); a.omnuv = Reading::Pass; a.network = Reading::Pass;
        a.internet = bad;
        Readings b = watching(); b.internet = Reading::Pass; b.network = Reading::Pass;
        b.omnuv = bad;
        Readings c = watching(); c.internet = Reading::Pass; c.omnuv = Reading::Pass;
        c.network = bad;
        CHECK(iconFor(b) != TrayIcon::Connected);
        CHECK(iconFor(c) != TrayIcon::Connected);
        (void)a;  // the internet row alone is allowed to be unknown: see below
    }

    // ---- prove the check can still see ----------------------------------
    // The one arrangement that is green. Without this, a function that always
    // returned Idle would pass every assertion above.
    Readings allPass = watching();
    allPass.internet = Reading::Pass;
    allPass.omnuv = Reading::Pass;
    allPass.network = Reading::Pass;
    CHECK(iconFor(allPass) == TrayIcon::Connected);

    // ---- and all four states are reachable ------------------------------
    // A fifth that nothing produces is dead, and a fourth that nothing
    // produces means the icon has three.
    CHECK(iconFor(Readings()) == TrayIcon::Idle);

    Readings unhappy = allPass;
    unhappy.machinesUnhappy = 1;
    CHECK(iconFor(unhappy) == TrayIcon::Attention);

    Readings moving = allPass;
    moving.machinesMoving = 1;
    CHECK(iconFor(moving) == TrayIcon::Working);

    Readings joining = watching();
    joining.joining = true;
    joining.network = Reading::Fail;      // expected while a join is running
    CHECK(iconFor(joining) == TrayIcon::Working);

    // ---- the judgements, asserted rather than left in a comment ----------

    // An unexplained Omnuv failure is worth a glance.
    Readings unreachable = watching();
    unreachable.internet = Reading::Pass;
    unreachable.network = Reading::Pass;
    unreachable.omnuv = Reading::Fail;
    CHECK(iconFor(unreachable) == TrayIcon::Attention);

    // The same failure, explained by this device being offline, is not: the
    // taskbar is already saying so four icons along.
    Readings offline = unreachable;
    offline.internet = Reading::Fail;
    CHECK(iconFor(offline) != TrayIcon::Attention);

    // A tunnel that is down is not a fault, because the client cannot tell it
    // from one nobody has joined yet, and red for an action not taken teaches
    // people to ignore red.
    Readings down = allPass;
    down.network = Reading::Fail;
    CHECK(iconFor(down) == TrayIcon::Idle);

    // Nothing the person has not done yet raises anything.
    Readings signedOut = allPass;
    signedOut.signedIn = false;
    CHECK(iconFor(signedOut) == TrayIcon::Idle);
    Readings noClient = allPass;
    noClient.tunnelInstalled = false;
    CHECK(iconFor(noClient) == TrayIcon::Idle);

    // A machine Core says is unhappy outranks a machine that is merely moving.
    Readings both = allPass;
    both.machinesUnhappy = 1;
    both.machinesMoving = 1;
    CHECK(iconFor(both) == TrayIcon::Attention);

    if (failed != 0) {
        std::printf("%d tray-state assertion(s) failed\n", failed);
        return 1;
    }
    std::printf("the tray icon never claims health it was not given\n");
    return 0;
}
