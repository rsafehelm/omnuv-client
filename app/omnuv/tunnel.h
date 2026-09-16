// Omnuv: the private network this device joins to reach machines by name.
//
// The implementation underneath is a third party's, running in a service of
// ours: `onvtunneld` is NetBird's own client hosted in a small daemon that
// owns the WireGuard adapter. This class asks it to join, to resume or for its
// state over a local socket, and reports one line.
//
// **Why a service.** Measured on 16 September: creating the adapter needs
// administrator rights, so the same code that joins in about twenty seconds
// from an elevated session hangs for ninety and dies with `context deadline
// exceeded` in the ordinary session the client starts in at login. The choice
// was between a UAC prompt every time somebody logs in and a privileged
// helper, and every WireGuard-family client on Windows answers it the same
// way. The daemon also means the machine is on its network *before* anyone
// logs in, and stays on it after the window is closed.
//
// Its name never appears on screen. A person joined *their Omnuv network*;
// which implementation carries the packets is not a thing they chose or should
// have to learn.

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTimer>

#include "traystate.h"

class OmnuvTunnel : public QObject
{
    Q_OBJECT

    // False when the service is not answering — not installed, not started,
    // or wedged. The view then says so rather than showing a button that
    // cannot work.
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
    ~OmnuvTunnel() override;

    bool available() const { return m_available; }
    bool connected() const { return m_reading == omnuv::Reading::Pass; }
    bool busy() const { return m_busy; }
    QString state() const { return m_state; }
    QString address() const { return m_address; }

    // The same fact as `connected()`, with the third answer this class used to
    // throw away.
    //
    // **A status that could not be read and a device that is off the network
    // were the same answer until today**, and that is the watcher defect in
    // its purest form: the old implementation killed `netbird status` at a
    // timeout, read nothing, and rendered "nothing" as the definite sentence
    // "This device is not on your network yet." A probe that could not run
    // reported a reading, and the reading was believed.
    //
    //   Pass     the service says the tunnel is up
    //   Fail     the service says it is not, and can say why
    //   Unknown  no service, or a join still in flight — nothing to report
    omnuv::Reading reading() const { return m_reading; }

    // When that reading was taken. Invalid before the first one, which renders
    // as nothing rather than as "just now".
    QDateTime takenAt() const { return m_takenAt; }

    // Re-read the service's own state. The only source of truth about whether
    // this device is on the network.
    Q_INVOKABLE void check();

    // Bring an already-enrolled device back up, and do nothing at all if it
    // has never enrolled.
    //
    // **This is what runs at startup, and the difference from join() is the
    // whole reason it exists.** `join()` answers "this person asked to be on
    // the network": when there is no identity it emits needsKey(), the session
    // fetches a one-time key, and a peer is created. Doing that automatically
    // would put every device that ever opened this application onto the
    // buyer's network without anyone asking — so the automatic path is the one
    // that can only ever *restore* something that already exists.
    Q_INVOKABLE void resume();

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
    void set(bool available, omnuv::Reading reading, const QString& state, const QString& address);
    void setBusy(bool busy);

    // A start the service refused, reported in the service's words when it
    // gave any.
    void fail(const QString& reply, const QString& fallback);

    // The poll runs while a view is watching **or** while a start is in
    // flight. The second half is not an optimisation: a resume at startup
    // happens with no view open, and without it the outcome of that start is
    // never observed — the state stays at whatever the first reading said and
    // the device is silently off the network.
    void updatePolling();

    // This device's address, read from the operating system rather than from
    // the service — with a real WireGuard adapter the embed API exposes
    // neither a status nor an address. See the note in tunnel.cpp.
    static QString adapterAddress();

    QTimer m_timer;
    bool m_watching = false;
    bool m_available = false;
    omnuv::Reading m_reading = omnuv::Reading::Unknown;
    bool m_busy = false;
    QString m_state;
    QString m_address;
    QDateTime m_takenAt;
};
