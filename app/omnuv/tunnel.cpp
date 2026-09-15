#include "tunnel.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibrary>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

namespace {

// The states the library reports. They are its numbers, repeated here rather
// than shared through a header, because the two halves meet at run time by
// name and nothing else — see the note on `exports()` below.
enum LibState { Stopped = 0, Starting = 1, Running = 2, Failed = 3 };

// And the codes `onv_tunnel_start` answers with. Only one of them is a
// question for the person: 3 means this device has never enrolled, so it has
// no identity to resume and somebody has to fetch it a one-time key.
enum StartCode { Accepted = 0, AlreadyUp = 1, Refused = 2, NeverEnrolled = 3 };

// The four exports of `onvtunnel`, resolved once.
//
// **Loaded by name, never linked.** The library is NetBird's own client built
// through cgo with mingw; this binary is built with MSVC. There is no import
// library to link against and none is wanted — resolving four plain C
// functions means the only things crossing the two C runtimes are integers and
// a buffer this side allocates and owns. Anything returning a `char*` would
// have to be freed by the wrong runtime, which is a real crash.
struct Exports
{
    int (*start)(const char*, const char*, const char*, const char*) = nullptr;
    int (*stop)() = nullptr;
    int (*state)() = nullptr;
    int (*lastError)(char*, int) = nullptr;

    bool ok() const { return start && stop && state && lastError; }
};

const Exports& exports()
{
    static const Exports resolved = [] {
        Exports e;
        // Beside the executable, because that is where the installer puts it.
        // A bare name would search the system and find whatever else is called
        // this.
        QLibrary lib(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("onvtunnel")));
        if (!lib.load()) {
            qWarning("omnuv: the private network is unavailable in this build: %s",
                     qPrintable(lib.errorString()));
            return e;
        }
        e.start = reinterpret_cast<decltype(e.start)>(lib.resolve("onv_tunnel_start"));
        e.stop = reinterpret_cast<decltype(e.stop)>(lib.resolve("onv_tunnel_stop"));
        e.state = reinterpret_cast<decltype(e.state)>(lib.resolve("onv_tunnel_state"));
        e.lastError = reinterpret_cast<decltype(e.lastError)>(lib.resolve("onv_tunnel_last_error"));
        if (!e.ok()) {
            // A library that loaded but is missing an export is a mismatched
            // build, and it must not read as "no network installed".
            qWarning("omnuv: the private network library is not the one this build expects");
            return e;
        }
        // **Said out loud, because the absence of a complaint is not a
        // reading.** A grader that passes a build where this class was never
        // constructed is the watcher defect, so there is a line to find rather
        // than a warning to miss.
        qInfo("omnuv: private network ready (%s)", qPrintable(lib.fileName()));
        return e;
    }();
    return resolved;
}

// The library's own account of the last failure, in its words. Copied into a
// buffer this side owns; see the note above.
QString libraryError()
{
    const Exports& e = exports();
    if (!e.ok()) {
        return QString();
    }
    char buf[512] = { 0 };
    const int n = e.lastError(buf, int(sizeof(buf)));
    return n > 0 ? QString::fromUtf8(buf, n) : QString();
}

// Where the identity lives between launches. **Not optional**: with no path
// the library keeps its configuration in memory, every launch enrols again,
// and the buyer's network collects one dead peer per launch.
QByteArray configDir()
{
    const QString dir =
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("network"));
    return QDir::toNativeSeparators(dir).toUtf8();
}

} // namespace

OmnuvTunnel::OmnuvTunnel(QObject* parent)
    : QObject(parent)
{
    // Three seconds rather than ten. Reading the state is now an integer
    // behind a mutex rather than a process to spawn, and a join takes tens of
    // seconds to resolve — so the view moves while it happens.
    m_timer.setInterval(3 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &OmnuvTunnel::check);
    check();
}

OmnuvTunnel::~OmnuvTunnel()
{
    // The adapter goes with the process either way; asking politely lets the
    // library take it down rather than leaving that to the operating system.
    const Exports& e = exports();
    if (e.ok()) {
        e.stop();
    }
}

// **The library cannot be asked where this device is.** With a real adapter
// (`NoUserspace`) the embed API exposes no status and no address: its status
// recorder is unexported, and the Dial/Listen calls that would know are
// netstack-only and unused here. So the address is read the way any other
// program on the machine would read it — from the operating system's own view
// of the interface the tunnel created.
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
    const Exports& e = exports();
    if (!e.ok()) {
        // Unknown rather than a failure, and the distinction is the whole
        // reason this enum has three values: there is nothing here to measure,
        // so nothing has been measured.
        set(false, omnuv::Reading::Unknown,
            tr("This build cannot reach your Omnuv network. Reinstalling Omnuv fixes it."),
            QString());
        return;
    }

    switch (e.state()) {
    case Running:
        // Whatever this device was waiting for, it is no longer waiting.
        setBusy(false);
        set(true, omnuv::Reading::Pass, tr("On your Omnuv network"), adapterAddress());
        return;

    case Starting:
        // Not an answer yet, and it must not read as one. Busy is left alone:
        // it belongs to whoever asked for the join.
        set(true, omnuv::Reading::Unknown, tr("Joining your Omnuv network…"), QString());
        return;

    case Failed: {
        setBusy(false);
        const QString why = libraryError();
        set(true, omnuv::Reading::Fail,
            why.isEmpty() ? tr("This device could not join your network.") : why, QString());
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

// Two steps that must not be confused. A device that enrolled before already
// holds an identity, and bringing it back up costs nothing and creates
// nothing. Only a device that has never enrolled needs a key — and a key
// spends itself, so asking for one when the device did not need it would leave
// a dead entry in the person's device list every time they opened this.
void OmnuvTunnel::join()
{
    const Exports& e = exports();
    if (m_busy || !e.ok()) {
        return;
    }
    setBusy(true);

    // No management URL and no key: *resume*. The stored configuration holds
    // both the server this device belongs to and the identity it enrolled
    // with, and the library refuses rather than inventing either.
    const QByteArray name = QSysInfo::machineHostName().toUtf8();
    const QByteArray dir = configDir();
    const int rc = e.start("", "", name.constData(), dir.constData());

    if (rc == NeverEnrolled) {
        // It has no identity, so it needs a one-time key. Whoever owns the
        // session fetches one and calls enrol; busy stays set until they do.
        emit needsKey();
        return;
    }
    if (rc != Accepted && rc != AlreadyUp) {
        fail(tr("This device could not rejoin your network."));
        return;
    }
    check();
}

void OmnuvTunnel::enrol(const QString& managementUrl, const QString& setupKey)
{
    const Exports& e = exports();
    if (!e.ok()) {
        return;
    }
    setBusy(true);

    const QByteArray url = managementUrl.toUtf8();
    const QByteArray key = setupKey.toUtf8();
    const QByteArray name = QSysInfo::machineHostName().toUtf8();
    const QByteArray dir = configDir();
    const int rc = e.start(url.constData(), key.constData(), name.constData(), dir.constData());

    if (rc != Accepted && rc != AlreadyUp) {
        fail(tr("This device could not join your network."));
        return;
    }
    check();
}

// A refusal the library gave a reason for. Its own sentence wins, because it
// names the actual obstacle — an unreachable server, a spent key, or the
// rights an adapter needs — and the generic one names none of those.
void OmnuvTunnel::fail(const QString& fallback)
{
    setBusy(false);
    const QString why = libraryError();
    set(true, omnuv::Reading::Fail, why.isEmpty() ? fallback : why, QString());
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
