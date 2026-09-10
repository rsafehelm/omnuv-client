// Omnuv: the private network this device joins to reach machines by name.
//
// The daemon underneath is a third party's, installed by the Omnuv Connect
// package rather than bundled inside this application — it needs a system
// service and administrator rights, which an application bundle should not be
// asking for. This class drives it and reports one line of state.
//
// Its name never appears on screen. A person joined *their Omnuv network*;
// which implementation carries the packets is not a thing they chose or should
// have to learn.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class OmnuvTunnel : public QObject
{
    Q_OBJECT

    // False when nothing is installed to drive. The view then says to install
    // Omnuv Connect rather than showing a button that cannot work.
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(bool connected READ connected NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)

    // One line, in a person's words. "Connected", "Not joined yet", or what
    // went wrong.
    Q_PROPERTY(QString state READ state NOTIFY changed)

    // This device's address on the project network, when it has one.
    Q_PROPERTY(QString address READ address NOTIFY changed)

public:
    explicit OmnuvTunnel(QObject* parent = nullptr);

    bool available() const { return m_available; }
    bool connected() const { return m_connected; }
    bool busy() const { return m_busy; }
    QString state() const { return m_state; }
    QString address() const { return m_address; }

    // Re-read the daemon's own status. The only source of truth about whether
    // this device is on the network.
    Q_INVOKABLE void check();

    // Put this device on the network. Brings an already-enrolled one back up;
    // emits needsKey() when it has no identity yet.
    Q_INVOKABLE void join();

    // Enrol with a one-time key, in answer to needsKey().
    void enrol(const QString& managementUrl, const QString& setupKey);

    // Stop waiting, and say why. For when a key could not be had.
    void giveUp(const QString& why);

    // Poll while a view is open; stop when it is not.
    Q_INVOKABLE void watch(bool on);

signals:
    void changed();

    // This device has never enrolled and needs a one-time key.
    void needsKey();

private:
    using Done = std::function<void(const QString& output, int exitCode)>;

    void set(bool available, bool connected, const QString& state, const QString& address);
    void setBusy(bool busy);
    static QString binary();

    // Runs one of the daemon's commands without blocking this thread, and
    // kills it if it outlives its budget.
    void start(const QStringList& args, int timeoutMs, Done done);

    QTimer m_timer;
    bool m_available = false;
    bool m_connected = false;
    bool m_busy = false;
    QString m_state;
    QString m_address;
};
