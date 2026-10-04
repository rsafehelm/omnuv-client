#include "paircli.h"
#include "linkcli.h"
#include "machinemodel.h"
#include "omnuvsession.h"

#include "backend/computermanager.h"
#include "cli/pair.h"
#include "streaming/streamutils.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

#include <functional>
#include <memory>

#include <stdio.h>

// The stdout contract, and the same rule as `signin` and `enrol`: this file is
// the only writer. Qt's logging goes to stderr and to the log file.
namespace {

void emitLine(const QString& line)
{
    fputs(qPrintable(line + QLatin1Char('\n')), stdout);
    fflush(stdout);
}

bool done = false;

void finish(int code)
{
    if (done) {
        return;
    }
    done = true;
    // Queued, because everything here runs before `app.exec()` and
    // `QCoreApplication::exit()` called before the loop starts does nothing at
    // all. Measured on `signin`, where it cost a run that sat for two minutes
    // and then printed a second verdict.
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

// **Pairing reaches a machine that may have only just booted.** The seek alone
// is ten seconds upstream, the machine has to answer its own HTTPS, and the
// delivery is a second request. Two minutes is the same budget `enrol` gets,
// and for the same reason: a hung run must end with a sentence rather than
// leave a harness waiting.
const int kDeadlineMs = 120 * 1000;

} // namespace

void OmnuvPairCli::start(const QStringList& args, QObject* parent)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Pair this device with a machine, without anybody typing a PIN."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("pair-machine"), QStringLiteral("The action."));
    parser.addPositionalArgument(QStringLiteral("host"),
                                 QStringLiteral("The machine's private name."));
    // The Core that owns the machine, for this run only; see signin.cpp. The
    // same flag `signin` and `enrol` take (B10), and for the same reason: a
    // device that once signed in to one deployment keeps that address saved,
    // and without the flag a harness pointed at another asked the saved one.
    QCommandLineOption coreUrlOption(
        QStringLiteral("core-url"),
        QStringLiteral("Ask this Core for the machine's login, for this run, without changing the saved one."),
        QStringLiteral("url"));
    parser.addOption(coreUrlOption);

    if (!parser.parse(args)) {
        fputs(qPrintable(parser.errorText() + QLatin1Char('\n')), stderr);
        ::exit(1);
    }
    if (parser.isSet(QStringLiteral("help"))) {
        parser.showHelp(0);
    }

    const QStringList positional = parser.positionalArguments();
    if (positional.size() < 2) {
        emitLine(QStringLiteral("state=failed  reason=name the machine to pair with"));
        ::exit(1);
    }
    QString host = positional.at(1);
    if (parser.isSet(coreUrlOption)) {
        OmnuvSession::setCoreUrlOverride(parser.value(coreUrlOption));
    }

    // **A session, because the credential is the whole point.** The machine's
    // one-time web login lives in Core, against the deployment that owns the
    // machine, and only a signed-in device may collect it. Without one this
    // action is upstream's `pair` with extra steps.
    auto* session = new OmnuvSession(parent);
    // Said before anything else, so a harness can hold it to the Core it
    // named rather than trust that the flag took.
    emitLine(QStringLiteral("core-url=%1").arg(session->coreUrl()));
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, session,
                     [session]() { session->finishPairing(); });
    if (!session->signedIn()) {
        emitLine(QStringLiteral("state=failed  reason=sign in first, so this device can collect "
                                "the machine's own login"));
        ::exit(1);
    }

    // One argument: upstream's constructor takes only the preferences, and
    // `main.cpp` leaks the same object for the `list` action. Parented to
    // `parent` afterwards so this one does not.
    // **The machine by id, in whichever project holds it** (the assets-by-id
    // audit, 3 October 2026). The target is an instance id, Core's host for
    // it, or a bare name only when exactly one live machine across the
    // account's projects has it; several are refused with their ids. This
    // replaces reading the project from the eight hex digits of a
    // `<name>-<project8>.internal` label, which are not an id, and matching
    // the delivery's row by host *or* name, which a machine made again under
    // a deleted one's name shares.
    QString machineId, projectId, why;
    {
        QString found;
        if (!OmnuvLinkCli::locateMachine(session, host, &machineId, &found, &projectId, &why)) {
            emitLine(QStringLiteral("state=failed  reason=%1").arg(why));
            ::exit(1);
        }
        if (found.isEmpty()) {
            emitLine(QStringLiteral("state=failed  reason=that machine has no address on your network yet"));
            ::exit(1);
        }
        host = found;
    }
    if (session->projectId() != projectId) {
        session->selectProject(projectId);
    }

    auto* computers = new ComputerManager(StreamingPreferences::get());
    computers->setParent(parent);

    // The PIN this run will use, decided here rather than read back from
    // anywhere. Upstream's launcher accepts a predefined one, which is what
    // makes the delivery possible without a screen in the middle.
    const QString pin = computers->generatePinString();

    auto* launcher = new CliPair::Launcher(host, pin, parent);

    // **The delivery hangs off `pairing`, and that is the machine's order
    // rather than ours.** The identifier a PIN is addressed to does not exist
    // until this client's own pairing request is already waiting on the
    // machine, so delivering beside `execute()` would address nothing. See
    // `pairing.cpp`.
    // **Finding the row waits for the list, because the list is fetched.**
    // `OmnuvSession` asks Core for its machines on construction and the answer
    // arrives whenever it arrives; the launcher's `pairing` signal arrives on
    // the machine's clock. On 16 September those two raced and the loser
    // reported `Omnuv does not list a machine at …`, which reads as a missing
    // machine and was a fetch that had not landed. Retried on every change to
    // the model, bounded by the run's own deadline.
    //
    // Matched on the machine's id, located above: the launcher reports what
    // Sunshine advertises (`gamerig-e2e`), which a namesake shares.
    auto deliver = std::make_shared<std::function<void()>>();
    auto delivered = std::make_shared<bool>(false);
    auto started = std::make_shared<bool>(false);
    *deliver = [session, machineId, pin, delivered, started]() {
        if (*delivered || !*started) {
            return;
        }
        MachineModel* machines = session->machines();
        for (int i = 0; i < machines->rowCount(); ++i) {
            if (machines->idAt(i) == machineId) {
                *delivered = true;
                session->deliverPin(session->connectionTarget(i), pin);
                return;
            }
        }
    };

    QObject::connect(launcher, &CliPair::Launcher::pairing, session,
                     [deliver, started](const QString& name, const QString&) {
                         *started = true;
                         emitLine(QStringLiteral("state=pairing  machine=%1").arg(name));
                         (*deliver)();
                     });

    // Every later arrival of the list is another chance, for as long as the
    // deadline allows. A machine that genuinely is not in the project simply
    // never matches, and the deadline says so.
    QObject::connect(session->machines(), &MachineModel::countChanged, session,
                     [deliver]() { (*deliver)(); });

    // **The delivery's success is announced, not only its failure.** Without
    // this the sequence `pairing → paired` looked complete while the PIN had
    // never been accepted, and the machine — which had an *open* pairing
    // session and no device — was the only thing that disagreed. A step that
    // reports one of two outcomes cannot be read as evidence of the other.
    QObject::connect(session, &OmnuvSession::pairingSucceeded, session,
                     []() { emitLine(QStringLiteral("state=delivered")); });

    // The delivery's own failure is reported and is *not* final: the machine
    // may still accept a PIN somebody types, so the launcher decides the
    // verdict. What this does is make the reason visible, which is the whole
    // difference between "pairing failed" and a diagnosis.
    QObject::connect(session, &OmnuvSession::pairingFailed, session,
                     [](const QString& why, const QString& detail) {
                         emitLine(QStringLiteral("state=delivery-failed  reason=%1%2")
                                      .arg(why, detail.isEmpty()
                                                    ? QString()
                                                    : QStringLiteral("  detail=") + detail));
                     });

    QObject::connect(launcher, &CliPair::Launcher::success, launcher,
                     []() { verdict(QStringLiteral("state=paired"), 0); });

    QObject::connect(launcher, &CliPair::Launcher::failed, launcher,
                     [](const QString& text) {
                         verdict(QStringLiteral("state=failed  reason=%1").arg(text), 1);
                     });

    // **The deadline says what it was waiting for.** Making the row lookup
    // retry silently turned "Omnuv does not list a machine" into two minutes
    // of nothing, which is the same defect this repository has now met in a
    // probe, a teardown and a watcher: an answer replaced by a silence. The
    // count and whether anything ever matched are the two facts that separate
    // a machine missing from the project, a fetch that never landed, and a
    // host that would not answer.
    QTimer::singleShot(kDeadlineMs, launcher, [session, host, delivered]() {
        verdict(QStringLiteral("state=failed  reason=the pairing did not finish in two minutes"
                               "  machines=%1  matched=%2  host=%3")
                    .arg(session->machines()->rowCount())
                    .arg(*delivered ? QStringLiteral("yes") : QStringLiteral("no"), host),
                1);
    });

    launcher->execute(computers);
}
