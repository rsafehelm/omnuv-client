#include "enrolcli.h"
#include "omnuvsession.h"
#include "tunnel.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <memory>
#include <stdio.h>

// ---------------------------------------------------------------------------
// The stdout contract
// ---------------------------------------------------------------------------
//
// One writer, and it is this file. Everything a person or a script reads goes
// out as `key=value` lines; Qt's own logging is on stderr and in the log file
// and never here. That is the same rule the rest of the product follows for
// machine-readable output, and it is why the lines below are written with
// `fputs` rather than `qDebug`.
//
//     state=joining
//     state=joined  address=100.72.0.14
//     state=failed  reason=<the library's own sentence>
//
// The address may be empty on a machine where the adapter came up without one
// yet; it is a fact reported when known, never invented.

namespace {

void emitLine(const QString& line)
{
    fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    fflush(stdout);
}

// **A verdict is final, and it has to survive not having an event loop yet.**
//
// Both of these were measured rather than reasoned about, on the first run of
// this action. `QCoreApplication::exit()` called before `exec()` does nothing
// at all — the loop has not started, so there is no loop to unwind — and the
// process then sat for the full two minutes and printed a second verdict from
// the deadline timer. Everything here runs before `app.exec()`, because that
// is how main.cpp dispatches an action, so the exit is *queued*: it is
// delivered the moment the loop starts, whether that is now or in a
// microsecond.
//
// And `done` is what makes the first answer the only one. A tunnel that has
// failed goes on reporting that it has failed, once every poll.
bool done = false;

// Polls spent waiting for the client to report the address it was given. See
// the `Pass` branch.
int addressPolls = 0;

void finish(int code)
{
    if (done) {
        return;
    }
    done = true;
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), [code]() { QCoreApplication::exit(code); },
        Qt::QueuedConnection);
}

void verdict(const QString& line, int code)
{
    if (done) {
        return;
    }
    emitLine(line);
    finish(code);
}

// **A join has to be able to fail by taking too long.** The library's own
// start carries a 90 second deadline; this is that plus room for the state to
// be observed, so a hung run ends with a sentence rather than with a script
// waiting forever.
const int kDeadlineMs = 120 * 1000;

} // namespace

void OmnuvEnrol::start(const QStringList& args, QObject* parent)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Join this device to its Omnuv private network."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("enrol"), QStringLiteral("The action."));
    parser.addPositionalArgument(QStringLiteral("key"),
                                 QStringLiteral("A one-time enrolment key. Omit it to bring an "
                                                "already-enrolled device back up."));
    QCommandLineOption urlOption(
        QStringLiteral("management-url"),
        QStringLiteral("The address of the network this device belongs to."),
        QStringLiteral("url"));
    parser.addOption(urlOption);
    // The Core whose sign-in this run uses to ask for a key, for this run
    // only; see signin.cpp.
    QCommandLineOption coreUrlOption(
        QStringLiteral("core-url"),
        QStringLiteral("Ask this Core for the key, for this run, without changing the saved one."),
        QStringLiteral("url"));
    parser.addOption(coreUrlOption);

    if (!parser.parse(args)) {
        fputs(qPrintable(parser.errorText() + QLatin1Char('\n')), stderr);
        ::exit(1);
    }
    if (parser.isSet(QStringLiteral("help"))) {
        parser.showHelp(0);
    }

    // args[0] is the executable and the first positional is the action itself.
    const QStringList positional = parser.positionalArguments();
    const QString key = positional.size() > 1 ? positional.at(1) : QString();
    const QString url = parser.value(urlOption);

    // **A key with no address is refused rather than guessed at.** The library
    // would fall back to the vendor's own cloud, enrol there, and report
    // success — a device on somebody else's network, which is worse than a
    // failure because it looks like one working.
    if (!key.isEmpty() && url.isEmpty()) {
        emitLine(QStringLiteral("state=failed  reason=no --management-url was given, and a key "
                                "cannot be spent without knowing where"));
        ::exit(1);
    }

    // **A session rather than a bare tunnel, and that is the whole point of
    // this change.** `OmnuvSession` already answers the tunnel's `needsKey()`
    // by asking Core for one — `/v1/networks`, then
    // `/v1/networks/{id}/devices` — with the token this device holds, and
    // reads the management address out of the command Core hands back rather
    // than guessing it. That is what the window does when a person presses
    // *Join*, so it is what a device signed in on this machine should do here.
    //
    // Passing a key on the command line still works and still goes straight to
    // the tunnel: it is the path the installer takes, where nobody has signed
    // in yet.
    // A supplied key is an explicit legacy enrollment. Do not let an unrelated
    // saved account's asynchronous identity read relabel or cancel that action.
    if (parser.isSet(coreUrlOption)) {
        OmnuvSession::setCoreUrlOverride(parser.value(coreUrlOption));
    }
    auto* session = key.isEmpty() ? new OmnuvSession(parent) : nullptr;
    if (session) emitLine(QStringLiteral("core-url=%1").arg(session->coreUrl()));
    OmnuvTunnel* tunnel = session ? session->tunnel() : new OmnuvTunnel(parent);

    auto requested = std::make_shared<bool>(!key.isEmpty());
    QObject::connect(tunnel, &OmnuvTunnel::changed, tunnel, [tunnel, requested]() {
        if (!*requested) return;
        switch (tunnel->reading()) {
        case omnuv::Reading::Pass:
            // **A short wait, and it is for the library rather than for the
            // machine.** Removing this was my own regression: the claim was
            // that `Status().LocalPeerState` holds the address at the instant
            // the client reports itself running. It does not quite — NetBird
            // reports running a moment before the recorder is populated, and
            // the run after that change printed `state=joined  address=` for a
            // tunnel that was up.
            //
            // The distinction that matters is *what is being waited for*.
            // Polling the operating system's interface list was waiting for a
            // side effect, which is the thing that must never be done. Asking
            // the client again for a value it will have shortly is waiting for
            // an answer, which is ordinary. Bounded, so a tunnel that stays
            // addressless still reports — `address=` with `state=joined` is a
            // finding, and saying nothing at all would say less.
            if (tunnel->address().isEmpty() && ++addressPolls < 8) {
                return;
            }
            verdict(QStringLiteral("state=joined  address=%1").arg(tunnel->address()), 0);
            return;
        case omnuv::Reading::Fail:
            // Only once it has stopped trying. While a join is in flight the
            // library reports `starting`, which arrives here as Unknown, and
            // the request below sets `busy` before anything is polled — so a
            // reading of "not on your network" taken *before* the attempt
            // cannot be mistaken for the attempt's result.
            if (!tunnel->busy()) {
                verdict(QStringLiteral("state=failed  reason=%1").arg(tunnel->state()), 1);
            }
            return;
        case omnuv::Reading::Unknown:
            return;
        }
    });

    // **`needsKey` is not connected here on purpose.** The session already
    // answers it, by fetching one from Core. What this watches instead is the
    // case where that fetch could not happen or did not work: the session
    // calls `giveUp()`, which leaves the reading Unknown with a sentence, and
    // the sentence is the answer — "Sign in first, so this device can be added
    // to your network" is a different failure from "Could not find your
    // network", and a harness that flattened both into "no key" would be
    // reporting its own guess.
    // The discriminator is the state machine, never the sentence: a join in
    // flight is *also* Unknown, and the only thing that separates it from a
    // give-up is that it is still busy. So a run that has been busy and is now
    // Unknown and idle has been given up on, whatever words came with it.
    auto wasBusy = std::make_shared<bool>(false);
    QObject::connect(tunnel, &OmnuvTunnel::changed, tunnel, [tunnel, wasBusy]() {
        if (tunnel->busy()) {
            *wasBusy = true;
            return;
        }
        if (*wasBusy && tunnel->reading() == omnuv::Reading::Unknown) {
            verdict(QStringLiteral("state=failed  reason=%1").arg(tunnel->state()), 1);
        }
    });

    QTimer::singleShot(kDeadlineMs, tunnel, []() {
        verdict(QStringLiteral("state=failed  reason=the join did not finish in two minutes"), 1);
    });

    emitLine(key.isEmpty() ? QStringLiteral("state=joining  mode=resume")
                           : QStringLiteral("state=joining  mode=enrol"));
    if (key.isEmpty() && session->signedIn()) {
        emitLine(QStringLiteral("state=joining  signed-in=yes"));
    }

    // **The request first, the polling second.** Both of them set `busy`
    // before they return, and the poll that `watch` takes immediately would
    // otherwise read the state of a device nobody had asked to do anything
    // yet — which is "not on your network", which is true and is not this
    // run's answer.
    if (key.isEmpty()) {
        // A saved token is not yet a verified account/project. Wait for /me
        // rather than asking the session to enroll against its default project.
        if (session->signedIn() && session->accountEmail().isEmpty()) {
            auto started = std::make_shared<bool>(false);
            QObject::connect(session, &OmnuvSession::projectsChanged, tunnel, [session, tunnel, started, requested]() {
                if (*started || session->accountEmail().isEmpty()) return;
                *started = true;
                *requested = true;
                tunnel->join();
            });
            emitLine(QStringLiteral("state=waiting  reason=reading selected account and project"));
        } else { *requested = true; tunnel->join(); }
    }
    else {
        tunnel->enrol(url, key);
    }
    // Polled, because the library's start is asynchronous and reports through
    // its state rather than through a return value.
    tunnel->watch(true);
}
