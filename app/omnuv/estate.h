// Omnuv: the estate — everything one project holds, and what became of it —
// as the window's three bands read it.
//
// The web console draws the same screen (`dashboard_v2.md`); this is the
// compact rendering of the same model, so it asks the same questions of the
// same endpoints and never a question of its own. Nothing here decides
// anything Core could decide: every value is Core's, carried to QML as it
// arrived, and every sentence a person reads about a change is built from
// Core's own fields in `estate.js`.
//
// **Every read has four states, and the difference is the point.** A failed
// read drawn as an empty list tells a person they own nothing; a failed
// refresh that blanks the rows tells them their machines vanished. So a read
// keeps what it last knew and says it could not refresh (`stale`), or says it
// never got an answer (`unavailable`) — never a zero, never an empty list
// wearing a fact's clothes. The console's `observe()` makes the same four
// promises, and the two are kept in the same words on purpose.

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QVariant>

#include <functional>

class QNetworkReply;

class OmnuvRead : public QObject
{
    Q_OBJECT

    // The body, parsed, exactly as Core sent it. Invalid until the first
    // answer, and kept through every failure after it.
    Q_PROPERTY(QVariant data READ data NOTIFY changed)

    //   loading      nothing asked has been answered yet
    //   current      the last answer arrived
    //   stale        an answer is held, and the latest refresh failed
    //   unavailable  nothing is held, and the latest read failed
    Q_PROPERTY(QString state READ state NOTIFY changed)

    // Why the latest read failed, in Core's words when Core gave any. Empty
    // while current.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)

    // When the held answer arrived. What "Showing 14:32" is drawn from.
    Q_PROPERTY(QDateTime readAt READ readAt NOTIFY changed)

public:
    using Get = std::function<QNetworkReply*(const QString& path)>;

    OmnuvRead(const QString& name, Get get, QObject* parent = nullptr);

    QVariant data() const { return m_data; }
    QString state() const { return m_state; }
    QString problem() const { return m_problem; }
    QDateTime readAt() const { return m_readAt; }

    // Ask `path`. **A different path is a different question**: what the old
    // one answered is dropped, and so is its reply if it is still in flight —
    // which is how a slow answer about the previous project is kept off a
    // screen that has moved on.
    void read(const QString& path);

    // Back to `loading` with nothing held, and anything in flight ignored.
    void forget();

    // The band's Retry: the same question again.
    Q_INVOKABLE void retry();

signals:
    void changed();

    // A current answer arrived. Only then: a dependent read (devices hang off
    // the network) must not be rebuilt from a failure.
    void landed();

private:
    void settle(const QString& state, const QString& problem);

    QString m_name;
    Get m_get;
    QString m_path;
    QVariant m_data;
    QString m_state = QStringLiteral("loading");
    QString m_problem;
    QDateTime m_readAt;
    quint64 m_seq = 0;
};

class OmnuvEstate : public QObject
{
    Q_OBJECT

    // Band one. From `/v1/me`, which the session already reads.
    Q_PROPERTY(QString organizationName READ organizationName NOTIFY identityChanged)
    Q_PROPERTY(bool owner READ owner NOTIFY identityChanged)

    Q_PROPERTY(OmnuvRead* members READ members CONSTANT)

    // Band two, beside the machines the session already lists.
    Q_PROPERTY(OmnuvRead* parked READ parked CONSTANT)
    Q_PROPERTY(OmnuvRead* networks READ networks CONSTANT)
    Q_PROPERTY(OmnuvRead* devices READ devices CONSTANT)
    Q_PROPERTY(OmnuvRead* endpoints READ endpoints CONSTANT)
    Q_PROPERTY(OmnuvRead* keys READ keys CONSTANT)
    Q_PROPERTY(OmnuvRead* usage READ usage CONSTANT)

    // Band three: the map's own history, newest first, collapsed to three.
    Q_PROPERTY(OmnuvRead* history READ history CONSTANT)

public:
    explicit OmnuvEstate(OmnuvRead::Get get, QObject* parent = nullptr);

    QString organizationName() const { return m_organizationName; }
    bool owner() const { return m_owner; }

    OmnuvRead* members() const { return m_members; }
    OmnuvRead* parked() const { return m_parked; }
    OmnuvRead* networks() const { return m_networks; }
    OmnuvRead* devices() const { return m_devices; }
    OmnuvRead* endpoints() const { return m_endpoints; }
    OmnuvRead* keys() const { return m_keys; }
    OmnuvRead* usage() const { return m_usage; }
    OmnuvRead* history() const { return m_history; }

    void setIdentity(const QString& organizationName, const QString& role);

    // Everything that belongs to a project is forgotten and asked again. The
    // organization's members are not: they do not change with the project.
    void setProject(const QString& projectId);

    // What moves — waiting requests and the history — on every call; the rest
    // on every fourth, because a network or an API key changes on the scale of
    // days and the window refreshes every minute.
    void refresh(bool everything);

    // Signed out: nothing of the last account stays on screen.
    void clear();

    // Every read's Retry, for the one banner that speaks for the band.
    Q_INVOKABLE void retryAll();

signals:
    void identityChanged();

private:
    void readAll();
    void readFast();
    void readSlow();

    OmnuvRead* m_members;
    OmnuvRead* m_parked;
    OmnuvRead* m_networks;
    OmnuvRead* m_devices;
    OmnuvRead* m_endpoints;
    OmnuvRead* m_keys;
    OmnuvRead* m_usage;
    OmnuvRead* m_history;

    QString m_organizationName;
    bool m_owner = false;
    QString m_project;
    int m_ticks = 0;
};
