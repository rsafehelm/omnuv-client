// Omnuv: the private network this device joins to reach machines by name.
//
// The implementation underneath is a third party's, and it is *inside this
// process*: `onvtunnel` is NetBird's own client compiled into a shared library
// that ships beside the executable, loaded by name at first use. There is no
// daemon to install, no second process to keep running, and nothing for a
// person to be told about. This class calls four functions and reports one
// line of state.
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

    // False when the library is not beside the binary or is not the one this
    // build expects. The view then says so rather than showing a button that
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
    //   Pass     the library says the tunnel is up
    //   Fail     the library says it is not, and can say why
    //   Unknown  no library, or a join still in flight — nothing to report
    omnuv::Reading reading() const { return m_reading; }

    // When that reading was taken. Invalid before the first one, which renders
    // as nothing rather than as "just now".
    QDateTime takenAt() const { return m_takenAt; }

    // Re-read the library's own state. The only source of truth about whether
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
    void set(bool available, omnuv::Reading reading, const QString& state, const QString& address);
    void setBusy(bool busy);

    // A start the library refused, reported in the library's words when it
    // gave any.
    void fail(const QString& fallback);

    // This device's address, read from the operating system rather than from
    // the library — with a real WireGuard adapter the embed API exposes
    // neither a status nor an address. See the note in tunnel.cpp.
    static QString adapterAddress();

    QTimer m_timer;
    bool m_available = false;
    omnuv::Reading m_reading = omnuv::Reading::Unknown;
    bool m_busy = false;
    QString m_state;
    QString m_address;
    QDateTime m_takenAt;
};
