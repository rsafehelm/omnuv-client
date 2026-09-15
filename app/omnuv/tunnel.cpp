#include "tunnel.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <memory>

// The daemon's own command line. Named here once, and nowhere else in the
// application, so replacing the implementation is one constant.
static const char* kBinary = "netbird";

// Bringing an enrolled device back up is quick. `up` with nothing to go on can
// sit forever waiting for a browser sign-in that this application never asked
// for, so it is killed rather than waited on.
static const int kResumeMs = 15 * 1000;
static const int kEnrolMs = 60 * 1000;

OmnuvTunnel::OmnuvTunnel(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(10 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &OmnuvTunnel::check);
    check();
}

QString OmnuvTunnel::binary()
{
    QString path = QStandardPaths::findExecutable(QString::fromLatin1(kBinary));
    if (!path.isEmpty()) {
        return path;
    }
#ifdef Q_OS_DARWIN
    // Not on a GUI application's PATH, which does not inherit a login shell.
    for (const QString& candidate : { QStringLiteral("/usr/local/bin/netbird"),
                                      QStringLiteral("/opt/homebrew/bin/netbird") }) {
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
#endif
    return QString();
}

// Every call into the daemon goes through here, and none of them blocks. A
// synchronous waitForFinished on this thread freezes the window, and the two
// commands worth running are exactly the slow ones.
void OmnuvTunnel::start(const QStringList& args, int timeoutMs, Done done)
{
    const QString path = binary();
    if (path.isEmpty()) {
        done(QString(), -1);
        return;
    }

    auto* p = new QProcess(this);
    auto* killer = new QTimer(p);
    killer->setSingleShot(true);
    killer->setInterval(timeoutMs);
    connect(killer, &QTimer::timeout, p, [p]() { p->kill(); });

    // finished and errorOccurred can both arrive for one run — a killed
    // process is the ordinary case. Calling back twice enrolled this device
    // twice and left a dead entry in the person's device list, so the callback
    // fires exactly once.
    auto fired = std::make_shared<bool>(false);
    auto finish = [p, done, fired](const QString& out, int code) {
        if (*fired) {
            return;
        }
        *fired = true;
        p->deleteLater();
        done(out, code);
    };

    // **The exit status is not the exit code, and conflating them is how a
    // killed probe reported a reading.** A process this class killed at its
    // timeout arrives here through `finished` like any other, with whatever
    // `exitCode` the platform invented for it — on Linux that is often 0. So
    // the status is folded in: anything but a clean exit is -1, the same
    // answer a process that never started gives, and `check()` treats -1 as
    // "could not look" rather than as an answer.
    connect(p, &QProcess::finished, this, [p, finish](int code, QProcess::ExitStatus status) {
        finish(QString::fromUtf8(p->readAllStandardOutput()),
               status == QProcess::NormalExit ? code : -1);
    });
    connect(p, &QProcess::errorOccurred, this, [p, finish](QProcess::ProcessError) {
        if (p->state() == QProcess::NotRunning) {
            finish(QString(), -1);
        }
    });

    p->start(path, args);
    killer->start();
}

void OmnuvTunnel::set(bool available, omnuv::Reading reading, const QString& state,
                      const QString& address)
{
    // The stamp moves on every reading, including one that changed nothing:
    // "we looked again and it is still true" is a different fact from "we have
    // not looked since then", and the tray renders the difference. So it is
    // set before the early return rather than after it.
    m_takenAt = QDateTime::currentDateTime();

    if (m_available == available && m_reading == reading && m_state == state
        && m_address == address) {
        // Still emitted, because the age moved even though nothing else did.
        emit changed();
        return;
    }
    m_available = available;
    m_reading = reading;
    m_state = state;
    m_address = address;
    emit changed();
}

void OmnuvTunnel::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    emit changed();
}

void OmnuvTunnel::check()
{
    if (binary().isEmpty()) {
        // Unknown rather than a failure, and the distinction is the whole
        // reason this enum has three values: nothing is installed, so nothing
        // has been measured. It is also an action the person has not taken,
        // and "an action the person has not taken yet is not a failure".
        set(false, omnuv::Reading::Unknown,
            tr("Install Omnuv Connect on this device to reach your machines by name."),
            QString());
        return;
    }

    // --json rather than the human output: the words there are the vendor's
    // and they change between versions.
    start({ QStringLiteral("status"), QStringLiteral("--json") }, 8000,
          [this](const QString& out, int code) {
              // **Three ways this can fail to be an answer, and all three used
              // to render as the definite sentence below.** The process was
              // killed at the timeout; it exited without printing; or it
              // printed something that is not the document we asked for. None
              // of those says where this device is — they say we could not
              // find out, which is a different thing and must look different.
              const QJsonObject o =
                  out.isEmpty() ? QJsonObject()
                                : QJsonDocument::fromJson(out.toUtf8()).object();

              if (code == -1 || o.isEmpty()) {
                  set(true, omnuv::Reading::Unknown,
                      tr("Could not read the network service on this device."),
                      QString());
                  return;
              }

              const bool up =
                  o[QStringLiteral("management")].toObject()[QStringLiteral("connected")].toBool();

              // Comes back with a prefix; a person wants the address.
              const QString address =
                  o[QStringLiteral("netbirdIp")].toString().section(QLatin1Char('/'), 0, 0);

              if (up) {
                  set(true, omnuv::Reading::Pass, tr("On your Omnuv network"), address);
              }
              else {
                  set(true, omnuv::Reading::Fail,
                      tr("This device is not on your network yet."), QString());
              }
          });
}

// Two steps that must not be confused. A device that enrolled before already
// holds an identity, and bringing it back up costs nothing and creates
// nothing. Only a device that has never enrolled needs a key — and a key
// spends itself, so asking for one when the device did not need it would leave
// a dead entry in the person's device list every time they opened this.
void OmnuvTunnel::join()
{
    if (m_busy) {
        return;
    }
    setBusy(true);

    start({ QStringLiteral("up") }, kResumeMs, [this](const QString&, int code) {
        if (code == 0) {
            setBusy(false);
            check();
            return;
        }
        // It has no identity, so it needs a one-time key. Whoever owns the
        // session fetches one and calls enrol; busy stays set until they do.
        emit needsKey();
    });
}

void OmnuvTunnel::enrol(const QString& managementUrl, const QString& setupKey)
{
    setBusy(true);
    start({ QStringLiteral("up"),
            QStringLiteral("--management-url"), managementUrl,
            QStringLiteral("--setup-key"), setupKey },
          kEnrolMs,
          [this](const QString&, int code) {
              setBusy(false);
              if (code != 0) {
                  set(m_available, omnuv::Reading::Fail,
                      tr("This device could not join. The network service may need "
                         "administrator rights."),
                      QString());
                  return;
              }
              check();
          });
}

void OmnuvTunnel::giveUp(const QString& why)
{
    setBusy(false);
    // A key could not be had, which is a thing that happened rather than a
    // thing we saw: this device's place on the network is exactly as unknown
    // as it was before we asked.
    set(m_available, omnuv::Reading::Unknown, why, QString());
}

void OmnuvTunnel::watch(bool on)
{
    if (on) {
        check();
        m_timer.start();
    }
    else {
        m_timer.stop();
    }
}
