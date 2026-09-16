#include "paircli.h"
#include "machinemodel.h"
#include "omnuvsession.h"

#include "backend/computermanager.h"
#include "cli/pair.h"
#include "streaming/streamutils.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTimer>

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
    QObject::connect(launcher, &CliPair::Launcher::pairing, session,
                     [session, host, pin](const QString& name, const QString&) {
                         emitLine(QStringLiteral("state=pairing  machine=%1").arg(name));

                         // `deliverPin` is addressed by a row of *Omnuv's* list
                         // of machines, which is what knows the deployment the
                         // credential belongs to — not upstream's list of
                         // hosts, which knows only an address.
                         MachineModel* machines = session->machines();
                         int row = -1;
                         for (int i = 0; i < machines->rowCount(); ++i) {
                             if (machines->hostAt(i).compare(host, Qt::CaseInsensitive) == 0) {
                                 row = i;
                                 break;
                             }
                         }
                         if (row < 0) {
                             verdict(QStringLiteral("state=failed  reason=Omnuv does not list a "
                                                    "machine at %1 in this project")
                                         .arg(host),
                                     1);
                             return;
                         }
                         session->deliverPin(row, pin);
                     });

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

    QTimer::singleShot(kDeadlineMs, launcher, []() {
        verdict(QStringLiteral("state=failed  reason=the pairing did not finish in two minutes"),
                1);
    });

    launcher->execute(computers);
}
