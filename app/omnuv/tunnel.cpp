#include "tunnel.h"

#include <QLocalSocket>
#include <QNetworkInterface>
#include <QTimer>

namespace {

// The states the daemon reports. They are its numbers, repeated here rather
// than shared through a header, because the two halves are separate programs
// that meet over a socket — one of them is Go — and the protocol is the only
// thing they have in common.
enum DaemonState { Stopped = 0, Starting = 1, Running = 2, Failed = 3 };

// Where `onvtunneld` listens. On Windows QLocalSocket takes the pipe's name
// without its `\\.\pipe\` prefix; everywhere else it is the socket's path.
QString serverName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("onv-tunnel");
#else
    const QString dir = qEnvironmentVariable("ONV_TUNNEL_DIR", QStringLiteral("/run"));
    return dir + QStringLiteral("/onv-tunnel.sock");
#endif
}

// One request, one answer, one connection.
//
// **Synchronous, deliberately, and what makes that allowable is that nothing
// here waits for a join.** The daemon accepts `resume` and `enrol` and returns
// immediately — the attempt runs in its own goroutine and the outcome arrives
// through `state`, which is a value it already holds. So the longest this
// blocks is a socket round trip on the local machine, and the timeouts below
// are for a daemon that is wedged rather than for any normal call. An empty
// return means it could not be reached, which is a third answer and is never
// the same as "not on the network".
QString request(const QString& line)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(300)) {
        return QString();
    }
    socket.write(line.toUtf8() + '\n');
    if (!socket.waitForBytesWritten(300) || !socket.waitForReadyRead(2000)) {
        return QString();
    }
    return QString::fromUtf8(socket.readAll()).trimmed();
}

} // namespace

OmnuvTunnel::OmnuvTunnel(QObject* parent)
    : QObject(parent)
{
    // Three seconds. Reading the state is a local socket round trip rather
    // than a process to spawn, and a join takes tens of seconds to resolve —
    // so the view moves while it happens.
    m_timer.setInterval(3 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &OmnuvTunnel::check);

    // **A device that has joined before is brought back up by the daemon, at
    // boot, before anybody logs in** — so there is nothing to start here. This
    // asks what is already true.
    check();
}

// **The daemon is not asked to stop.** It holds the tunnel for the machine,
// not for this window: closing the client must not take a buyer's network
// down, any more than closing a browser turns off their Wi-Fi.
OmnuvTunnel::~OmnuvTunnel() = default;

// **The daemon cannot be asked where this device is.** With a real adapter
// (`NoUserspace`) the embed API exposes neither a status nor an address: its
// status recorder is unexported, and the Dial/Listen calls that would know are
// netstack-only and unused here. So the address is read the way any other
// program on the machine would read it — from the operating system's own view
// of the interface the tunnel created. That also makes it a *second* source:
// the daemon says whether it believes it is up, the machine says whether there
// is an adapter, and a disagreement between them is a finding.
QString OmnuvTunnel::adapterAddress()
{
#ifdef Q_OS_DARWIN
    static const QString kAdapter = QStringLiteral("utun100");
#else
    static const QString kAdapter = QStringLiteral("wt0");
#endif
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : all) {
        // Windows names an interface by its GUID and keeps the friendly name
        // separately; Linux and macOS put it in both.
        if (iface.name() != kAdapter && iface.humanReadableName() != kAdapter) {
            continue;
        }
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry& entry : entries) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                return entry.ip().toString();
            }
        }
    }
    return QString();
}

void OmnuvTunnel::set(bool available, omnuv::Reading reading, const QString& state,
                      const QString& address)
{
    // The stamp moves on every reading, including one that changed nothing:
    // "we looked again and it is still true" is a different fact from "we have
    // not looked since then", and the tray renders the difference. So it is
    // set before the early return rather than after it.
    m_takenAt = QDateTime::currentDateTime();

    // **Said once, where a failure can be read later.** The sentence the
    // daemon gives is shown in the window, and a window is not somewhere a
    // support case or a rig can look. Logged only on a change, because a
    // failed tunnel repeats its failure every poll and a log that repeats is a
    // log nobody greps.
    if (reading != m_reading || state != m_state) {
        if (reading == omnuv::Reading::Fail) {
            qWarning("omnuv: private network: %s", qPrintable(state));
        }
        else if (reading == omnuv::Reading::Pass) {
            qInfo("omnuv: private network up at %s",
                  address.isEmpty() ? "an address it could not read" : qPrintable(address));
        }
        else {
            // Including this one. "Still starting" is a reading, and a log
            // that only records verdicts cannot show a thing that never
            // reached one.
            qInfo("omnuv: private network: %s", qPrintable(state));
        }
    }

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
    updatePolling();
    emit changed();
}

void OmnuvTunnel::updatePolling()
{
    const bool wanted = m_watching || m_busy;
    if (wanted && !m_timer.isActive()) {
        m_timer.start();
    }
    else if (!wanted && m_timer.isActive()) {
        m_timer.stop();
    }
}

void OmnuvTunnel::check()
{
    const QString reply = request(QStringLiteral("state"));

    if (reply.isEmpty()) {
        // Unknown rather than a failure, and the distinction is the whole
        // reason this enum has three values: nothing answered, so nothing has
        // been measured. It is also the shape of a perfectly ordinary machine
        // — one where the service is not installed — and that must not read as
        // "you are not on your network".
        set(false, omnuv::Reading::Unknown,
            tr("The Omnuv network service is not running on this device."), QString());
        return;
    }

    // `state <n> <sentence>`; the sentence may be empty and may contain
    // spaces, so it is everything after the second field.
    const QStringList fields = reply.split(QLatin1Char(' '));
    const int state = fields.value(1).toInt();
    const QString why = fields.mid(2).join(QLatin1Char(' '));

    switch (state) {
    case Running: {
        // Whatever this device was waiting for, it is no longer waiting.
        setBusy(false);
        set(true, omnuv::Reading::Pass, tr("On your Omnuv network"), adapterAddress());
        return;
    }

    case Starting:
        // Not an answer yet, and it must not read as one. Busy is left alone:
        // it belongs to whoever asked for the join.
        set(true, omnuv::Reading::Unknown, tr("Joining your Omnuv network…"), QString());
        return;

    case Failed:
        setBusy(false);
        set(true, omnuv::Reading::Fail,
            why.isEmpty() ? tr("This device could not join your network.") : why, QString());
        return;

    default:
        // Stopped. Busy is deliberately not cleared here: a device that has
        // never enrolled sits in this state while somebody fetches it a key,
        // and clearing it would offer the join button again mid-flight.
        set(true, omnuv::Reading::Fail, tr("This device is not on your network yet."), QString());
        return;
    }
}

// Two steps that must not be confused. A device that enrolled before already
// holds an identity, and bringing it back up costs nothing and creates
// nothing. Only a device that has never enrolled needs a key — and a key
// spends itself, so asking for one when the device did not need it would leave
// a dead entry in the person's device list every time they opened this.
//
// resume() and join() send the same request and differ only in what they do
// with `err need-key`, which is the daemon's way of saying this machine has no
// identity to restore. That answer is a question for a person, and only one of
// the two was asked by one.
void OmnuvTunnel::resume()
{
    if (m_busy) {
        return;
    }
    setBusy(true);
    const QString reply = request(QStringLiteral("resume"));

    if (reply.startsWith(QLatin1String("err need-key"))) {
        // Nothing to restore, and nothing to ask: this is the ordinary state
        // of a device nobody has put on a network yet. `busy` goes back down
        // and the view keeps offering the join it was already offering.
        setBusy(false);
        check();
        return;
    }
    if (!reply.startsWith(QLatin1String("ok"))) {
        fail(reply, tr("This device could not rejoin your network."));
        return;
    }
    check();
}

void OmnuvTunnel::join()
{
    if (m_busy) {
        return;
    }
    setBusy(true);
    const QString reply = request(QStringLiteral("resume"));

    if (reply.startsWith(QLatin1String("err need-key"))) {
        // It has no identity, so it needs a one-time key. Whoever owns the
        // session fetches one and calls enrol; busy stays set until they do.
        emit needsKey();
        return;
    }
    if (!reply.startsWith(QLatin1String("ok"))) {
        fail(reply, tr("This device could not rejoin your network."));
        return;
    }
    check();
}

void OmnuvTunnel::enrol(const QString& managementUrl, const QString& setupKey)
{
    setBusy(true);
    const QString reply = request(QStringLiteral("enrol %1 %2").arg(managementUrl, setupKey));

    if (!reply.startsWith(QLatin1String("ok"))) {
        fail(reply, tr("This device could not join your network."));
        return;
    }
    check();
}

// A refusal the daemon gave a reason for. Its own sentence wins, because it
// names the actual obstacle — an unreachable server, a spent key, or the
// rights an adapter needs — and the generic one names none of those.
void OmnuvTunnel::fail(const QString& reply, const QString& fallback)
{
    setBusy(false);
    QString why = reply;
    if (why.startsWith(QLatin1String("err "))) {
        why = why.mid(4);
    }
    // An empty reply is the service not answering at all, which is the one
    // case where `available` goes back down.
    set(!reply.isEmpty(), omnuv::Reading::Fail, why.isEmpty() ? fallback : why, QString());
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
    m_watching = on;
    if (on) {
        check();
    }
    updatePolling();
}
