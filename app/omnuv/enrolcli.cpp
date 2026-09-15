#include "enrolcli.h"
#include "tunnel.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

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

    auto* tunnel = new OmnuvTunnel(parent);

    QObject::connect(tunnel, &OmnuvTunnel::changed, tunnel, [tunnel]() {
        switch (tunnel->reading()) {
        case omnuv::Reading::Pass:
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

    // A device that has never enrolled, asked to resume. The window answers
    // this by fetching a key from Core; a shell cannot, so it says what is
    // missing in the words somebody can act on.
    QObject::connect(tunnel, &OmnuvTunnel::needsKey, tunnel, []() {
        verdict(QStringLiteral("state=failed  reason=this device has never joined a network, so "
                               "it needs a one-time key: enrol <key> --management-url <url>"),
                1);
    });

    QTimer::singleShot(kDeadlineMs, tunnel, []() {
        verdict(QStringLiteral("state=failed  reason=the join did not finish in two minutes"), 1);
    });

    emitLine(key.isEmpty() ? QStringLiteral("state=joining  mode=resume")
                           : QStringLiteral("state=joining  mode=enrol"));

    // **The request first, the polling second.** Both of them set `busy`
    // before they return, and the poll that `watch` takes immediately would
    // otherwise read the state of a device nobody had asked to do anything
    // yet — which is "not on your network", which is true and is not this
    // run's answer.
    if (key.isEmpty()) {
        tunnel->join();
    }
    else {
        tunnel->enrol(url, key);
    }
    // Polled, because the library's start is asynchronous and reports through
    // its state rather than through a return value.
    tunnel->watch(true);
}
