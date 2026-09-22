#include "tunnel.h"

#include <QLocalSocket>
#include <QJsonDocument>
#include <QDebug>
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
    if (qEnvironmentVariableIsSet("OMNUV_FIXTURE_URL")
        && line != QLatin1String("state") && !line.startsWith(QLatin1String("membership-v1")))
        return QStringLiteral("err Fixture sessions cannot change this device's network membership.");
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
    : QObject(parent), m_request(request)
{
    // Three seconds. Reading the state is a local socket round trip rather
    // than a process to spawn, and a join takes tens of seconds to resolve —
    // so the view moves while it happens.
    m_timer.setInterval(3 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &OmnuvTunnel::check);
    m_operationDeadline.setSingleShot(true);
    m_operationDeadline.setInterval(120000);
    connect(&m_operationDeadline, &QTimer::timeout, this, [this]() {
        if (!m_busy) return;
        check(); // a deadline asks for another observation, never assumes failure
        if (m_busy) {
            emit observationExpired();
            giveUp(tr("Enrollment was not confirmed before the waiting period ended. Recheck the saved attempt or revoke it before starting over."));
        }
    });

    // **A device that has joined before is brought back up by the daemon, at
    // boot, before anybody logs in** — so there is nothing to start here. This
    // asks what is already true.
    check();
}

// **The daemon is not asked to stop.** It holds the tunnel for the machine,
// not for this window: closing the client must not take a buyer's network
// down, any more than closing a browser turns off their Wi-Fi.
OmnuvTunnel::~OmnuvTunnel() = default;

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

void OmnuvTunnel::setOperationError(const QString& why)
{
    if (m_operationError == why) return;
    m_operationError = why;
    if (!why.isEmpty()) qWarning().noquote() << "omnuv: network enrollment:" << why;
    emit changed();
}

void OmnuvTunnel::clearOperationError() { setOperationError(QString()); }

void OmnuvTunnel::setBusy(bool busy)
{
    if (m_busy == busy) {
        return;
    }
    m_busy = busy;
    if (busy) m_operationDeadline.start(); else m_operationDeadline.stop();
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

// **The address comes from the daemon, which asks the library.** This used to
// enumerate the machine's interfaces and look for `wt0`, and the failure that
// removed it is worth keeping: the adapter is configured a moment *after* the
// engine reports itself up, so a read taken at the wrong instant returned an
// empty string for a tunnel that was working. The rig printed
// `state=joined  address=` while its own guest agent showed `wt0` holding
// `10.210.219.11`.
//
// NetBird's `Client.Status().LocalPeerState` carries the address the
// management server assigned and the name it published, so the daemon reports
// both on the `state` line. **Where a library answers the question, ask the
// library** — an observation of its side effects is a different fact, arriving
// later and for other reasons.
void OmnuvTunnel::check()
{
    readMembership();
    const QString reply = m_request(QStringLiteral("state"));

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

    // `state <n> <address|-> <name|-> <sentence>`; the sentence may be empty
    // and may contain spaces, so it is everything after the fourth field.
    const QStringList fields = reply.split(QLatin1Char(' '));
    const int state = m_membershipSupported ? m_membershipSnapshot.value("state").toInt() : fields.value(1).toInt();
    const QString dashed = fields.value(2);
    const QString address = m_membershipSupported ? m_membershipSnapshot.value("address").toString() : (dashed == QLatin1String("-") ? QString() : dashed);
    const QString why = m_membershipSupported ? m_membershipSnapshot.value("error").toString() : fields.mid(4).join(QLatin1Char(' '));

    switch (state) {
    case Running: {
        if (m_waitingForKey) {
            set(true, omnuv::Reading::Unknown, tr("Checking the selected network membership…"), QString());
            return;
        }
        if (!m_legacyAction && (!m_membershipVerified || !membershipMatches(m_scope))) {
            set(true, omnuv::Reading::Unknown,
                m_membershipSupported
                    ? tr("This device is connected to another or unverified network. Its current identity has been kept.")
                    : tr("Update the Omnuv network service to verify which project this device joined."), QString());
            return;
        }
        if (m_busy && !m_waitingForKey) clearOperationError();
        // **The state first, then busy.** `setBusy` emits `changed()`, so
        // clearing it before the reading is written publishes one moment where
        // the tunnel is idle and still carrying the *previous* sentence — and
        // a watcher reading that pair reported "failed: Joining your Omnuv
        // network", which is two states mixed into one lie.
        set(true, omnuv::Reading::Pass, tr("On your Omnuv network"), address);
        if (!m_waitingForKey) setBusy(false);
        return;
    }

    case Starting:
        // Not an answer yet, and it must not read as one. Busy is left alone:
        // it belongs to whoever asked for the join.
        set(true, omnuv::Reading::Unknown, tr("Joining your Omnuv network…"), QString());
        return;

    case Failed: {
        // Revocation is never repaired by silently creating another device.
        // A saved attempt remains recoverable through explicit revocation.
        const bool refused = why.startsWith(QLatin1String("unauthorized:"));
        // A key the overlay would not take is not a membership taken away:
        // it was spent already, or it expired (an hour, since H8). Saying
        // "no longer a member" sent people looking for a revocation.
        const bool spentKey = refused && why.contains(QLatin1String("setup-key"), Qt::CaseInsensitive);

        if (m_busy && !m_waitingForKey) setOperationError(why.isEmpty() ? tr("This device could not join your network.") : why);
        // The state first, then busy — see the Running branch above.
        set(true, omnuv::Reading::Fail,
            why.isEmpty() ? tr("This device could not join your network.")
                          : (spentKey ? tr("That network key was already used or has expired. Join again for a new one.")
                             : refused ? tr("This device is no longer a member of your network.")
                                       : why),
            QString());
        if (!m_waitingForKey) setBusy(false);

        return;
    }

    default:
        // Stopped. Busy is deliberately not cleared here: a device that has
        // never enrolled sits in this state while somebody fetches it a key,
        // and clearing it would offer the join button again mid-flight.
        set(true, omnuv::Reading::Fail, tr("This device is not on your network yet."), QString());
        return;
    }
}

// An application opening is never permission to change the machine's membership.
// The daemon may restore its own identity at boot; a client resume must name it.
void OmnuvTunnel::resume()
{
    if (m_busy || !readMembership() || !membershipMatches(m_scope)) return;
    resumeMembership(m_membership);
}

void OmnuvTunnel::join()
{
    if (m_busy) return;
    m_legacyAction = false;
    m_waitingForKey = true;
    clearOperationError();
    setBusy(true);
    // Core/account/project and the project's network must be known before
    // deciding whether a resume is safe. Never blindly resume a live identity.
    emit needsKey();
}

void OmnuvTunnel::setScope(const QJsonObject& scope)
{
    m_scope = scope;
    m_legacyAction = false;
    check();
}

bool OmnuvTunnel::membershipMatches(const QJsonObject& scope) const
{
    if (!m_membershipSupported || m_membership.isEmpty() || scope.isEmpty()) return false;
    for (const auto key : {"core_url", "account_id", "project_id"})
        if (scope.value(key).toString().isEmpty()) return false;
    for (auto it = scope.begin(); it != scope.end(); ++it)
        if (it.value() != m_membership.value(it.key())) return false;
    return true;
}

bool OmnuvTunnel::readMembership()
{
    // **This Core's identity, not whichever is running** (22 September 2026).
    // The daemon keeps one identity per deployment and runs one at a time, so
    // after a move between Cores the running one is the other Core's: read
    // unqualified, it looked like "another network" and offered a move, which
    // re-enrolled a device whose identity for this Core was saved all along.
    // An older daemon answers "takes no arguments", and is asked the old way.
    const QString core = m_scope.value(QStringLiteral("core_url")).toString();
    QString reply = m_request(core.isEmpty() ? QStringLiteral("membership-v1")
                                             : QStringLiteral("membership-v1 ") + core);
    if (!core.isEmpty() && reply.startsWith(QLatin1String("err membership-v1 takes no arguments")))
        reply = m_request(QStringLiteral("membership-v1"));
    const auto document = QJsonDocument::fromJson(reply.toUtf8());
    const auto value = document.object();
    m_membershipSnapshot = value;
    m_membershipSupported = document.isObject() && value.value("version").toInt() == 1
        && value.value("state").isDouble() && value.value("state").toInt(-1) >= 0 && value.value("state").toInt(-1) <= 3
        && value.value("verified").isBool() && value.value("has_identity").isBool() && !value.value("revision").toString().isEmpty()
        && (value.value("membership").isNull() || value.value("membership").isObject());
    m_membership = m_membershipSupported ? value.value("membership").toObject() : QJsonObject{};
    if (!m_membership.isEmpty()) {
        for (const auto key : {"core_url", "account_id", "project_id", "network_id", "device_id"})
            if (m_membership.value(key).toString().isEmpty()) m_membershipSupported = false;
    }
    m_hasIdentity = m_membershipSupported && value.value("has_identity").toBool();
    m_membershipVerified = m_membershipSupported && value.value("verified").toBool();
    m_membershipRevision = m_membershipSupported ? value.value("revision").toString() : QString();
    return m_membershipSupported;
}

namespace {
QString membershipCommand(const QString& verb, const QJsonObject& body)
{
    return verb + QLatin1Char(' ') + QString::fromLatin1(QJsonDocument(body).toJson(QJsonDocument::Compact).toBase64());
}
}

void OmnuvTunnel::resumeMembership(const QJsonObject& membership)
{
    m_scope = membership;
    m_waitingForKey = false;
    clearOperationError();
    setBusy(true);
    const auto reply = m_request(membershipCommand(QStringLiteral("resume-v1"), membership));
    if (reply != QLatin1String("ok")) { fail(reply, tr("The saved network membership could not be resumed.")); return; }
    check();
}

void OmnuvTunnel::enrolMembership(const QString& managementUrl, const QString& setupKey,
                                 const QJsonObject& membership, const QString& expectedRevision)
{
    m_scope = membership;
    m_waitingForKey = false;
    clearOperationError();
    setBusy(true);
    const auto reply = m_request(membershipCommand(QStringLiteral("enrol-v1"),
        {{"membership", membership}, {"management_url", managementUrl},
         {"setup_key", setupKey}, {"expected_revision", expectedRevision}}));
    if (reply != QLatin1String("ok")) { fail(reply, tr("The network service did not accept this enrollment. Revoke the pending enrollment before retrying.")); return; }
    check();
}

bool OmnuvTunnel::stopMembership(const QJsonObject& membership)
{
    if (!readMembership()) { giveUp(tr("The network service cannot verify the enrollment to stop.")); return false; }
    if (m_membership != membership) return true; // Another membership is never stopped.
    const auto reply = m_request(membershipCommand(QStringLiteral("stop-v1"),
        {{"membership", membership}, {"expected_revision", m_membershipRevision}}));
    if (reply != QLatin1String("ok")) { fail(reply, tr("Access was revoked, but this device could not stop its network service. Retry cleanup.")); return false; }
    check();
    return true;
}

void OmnuvTunnel::enrol(const QString& managementUrl, const QString& setupKey)
{
    m_legacyAction = true;
    clearOperationError();
    m_userAsked = true;
    setBusy(true);
    const QString reply = m_request(QStringLiteral("enrol %1 %2").arg(managementUrl, setupKey));

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
    m_waitingForKey = false;
    QString why = reply;
    if (why.startsWith(QLatin1String("err "))) {
        why = why.mid(4);
    }
    // An empty reply is the service not answering at all, which is the one
    // case where `available` goes back down.
    setOperationError(why.isEmpty() ? fallback : why);
    set(!reply.isEmpty(), omnuv::Reading::Fail, m_operationError, QString());
    setBusy(false);
}

void OmnuvTunnel::beginOperation(const QString& why)
{
    m_waitingForKey = true;
    set(true, omnuv::Reading::Unknown, why, QString());
    setBusy(true);
}

void OmnuvTunnel::giveUp(const QString& why)
{
    m_waitingForKey = false;
    setOperationError(why);
    // A key could not be had, which is a thing that happened rather than a
    // thing we saw: this device's place on the network is exactly as unknown
    // as it was before we asked.
    set(m_available, omnuv::Reading::Unknown, why, QString());
    setBusy(false);
}

void OmnuvTunnel::watch(bool on)
{
    m_watching = on;
    if (on) {
        check();
    }
    updatePolling();
}
