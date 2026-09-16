#include "paircli.h"
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
    const QString host = positional.at(1);

    // **A session, because the credential is the whole point.** The machine's
    // one-time web login lives in Core, against the deployment that owns the
    // machine, and only a signed-in device may collect it. Without one this
    // action is upstream's `pair` with extra steps.
    auto* session = new OmnuvSession(parent);
    if (!session->signedIn()) {
        emitLine(QStringLiteral("state=failed  reason=sign in first, so this device can collect "
                                "the machine's own login"));
        ::exit(1);
    }

    // One argument: upstream's constructor takes only the preferences, and
    // `main.cpp` leaks the same object for the `list` action. Parented to
    // `parent` afterwards so this one does not.
    // **The machine's name says which project it is in, so use it.** A
    // marketplace private name is `<machine>-<project8>.internal`, and a
    // device signed in to an account with several projects otherwise picks one
    // by an order that has nothing to do with the machine being asked for.
    // Measured on 16 September: the account held five live projects, the
    // client took the oldest, and it reported `0 machine(s)` while the machine
    // sat in the newest — so no row ever matched and no PIN was ever
    // delivered, for two minutes, silently.
    //
    // Selecting it here rather than fixing the order elsewhere, because the
    // order is a fair default for a person opening a window and is simply not
    // an answer to *this* question: the caller named a machine.
    const QString label = host.section(QLatin1Char('.'), 0, 0);
    const QString suffix = label.section(QLatin1Char('-'), -1);
    QObject::connect(session, &OmnuvSession::projectsChanged, session, [session, suffix]() {
        if (suffix.length() != 8) {
            return;
        }
        for (const QString& id : session->projectIds()) {
            if (id.startsWith(suffix, Qt::CaseInsensitive) && id != session->projectId()) {
                session->selectProject(id);
                return;
            }
        }
    });

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
    // Matched on the host *or* the name, because the launcher reports what
    // Sunshine advertises — `gamerig-e2e` — while the harness asks for
    // `gamerig-e2e-<project8>.internal`, and either is a fair way to say which
    // machine this is.
    auto deliver = std::make_shared<std::function<void()>>();
    auto delivered = std::make_shared<bool>(false);
    *deliver = [session, host, pin, delivered]() {
        if (*delivered) {
            return;
        }
        MachineModel* machines = session->machines();
        const QString bare = host.section(QLatin1Char('.'), 0, 0);
        for (int i = 0; i < machines->rowCount(); ++i) {
            if (machines->hostAt(i).compare(host, Qt::CaseInsensitive) == 0
                || machines->nameAt(i).compare(bare, Qt::CaseInsensitive) == 0) {
                *delivered = true;
                session->deliverPin(i, pin);
                return;
            }
        }
    };

    QObject::connect(launcher, &CliPair::Launcher::pairing, session,
                     [deliver](const QString& name, const QString&) {
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
