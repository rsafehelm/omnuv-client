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

    connect(p, &QProcess::finished, this, [p, finish](int code, QProcess::ExitStatus) {
        finish(QString::fromUtf8(p->readAllStandardOutput()), code);
    });
    connect(p, &QProcess::errorOccurred, this, [p, finish](QProcess::ProcessError) {
        if (p->state() == QProcess::NotRunning) {
            finish(QString(), -1);
        }
    });

    p->start(path, args);
    killer->start();
}

void OmnuvTunnel::set(bool available, bool connected, const QString& state, const QString& address)
{
    if (m_available == available && m_connected == connected && m_state == state
        && m_address == address) {
        return;
    }
    m_available = available;
    m_connected = connected;
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
        set(false, false,
            tr("Install Omnuv Connect on this device to reach your machines by name."),
            QString());
        return;
    }

    // --json rather than the human output: the words there are the vendor's
    // and they change between versions.
    start({ QStringLiteral("status"), QStringLiteral("--json") }, 8000,
          [this](const QString& out, int) {
              if (out.isEmpty()) {
                  set(true, false, tr("This device is not on your network yet."), QString());
                  return;
              }

              const QJsonObject o = QJsonDocument::fromJson(out.toUtf8()).object();
              const bool up =
                  o[QStringLiteral("management")].toObject()[QStringLiteral("connected")].toBool();

              // Comes back with a prefix; a person wants the address.
              const QString address =
                  o[QStringLiteral("netbirdIp")].toString().section(QLatin1Char('/'), 0, 0);

              if (up) {
                  set(true, true, tr("On your Omnuv network"), address);
              }
              else {
                  set(true, false, tr("This device is not on your network yet."), QString());
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
                  set(m_available, false,
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
    set(m_available, false, why, QString());
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
