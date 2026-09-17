#include "estate.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>

OmnuvRead::OmnuvRead(const QString& name, Get get, QObject* parent)
    : QObject(parent), m_name(name), m_get(std::move(get))
{
}

void OmnuvRead::read(const QString& path)
{
    if (path != m_path) {
        forget();
        m_path = path;
    }

    // The sequence number is what drops a late answer: only the newest
    // question may land, and `forget()` moves it on without asking anything.
    const quint64 seq = ++m_seq;
    QNetworkReply* reply = m_get(path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, seq]() {
        reply->deleteLater();
        if (seq != m_seq) {
            return;
        }

        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (reply->error() == QNetworkReply::NoError) {
            QJsonParseError parsed;
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parsed);
            if (parsed.error == QJsonParseError::NoError) {
                m_data = doc.toVariant();
                m_readAt = QDateTime::currentDateTime();
                settle(QStringLiteral("current"), QString());
                emit landed();
                return;
            }
            settle(m_data.isValid() ? QStringLiteral("stale") : QStringLiteral("unavailable"),
                   tr("Omnuv answered with something this version cannot read."));
            return;
        }

        // Core's own sentence first: it says `{"error": "..."}` and, on a
        // 500, carries the reference a person can quote. Ours only when there
        // is nothing of Core's to show, and then it says which of the two
        // things happened rather than a status number.
        QString why = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString().trimmed();
        if (why.isEmpty()) {
            why = code == 0 ? tr("Omnuv could not be reached.")
                            : tr("Omnuv could not answer this just now.");
        }
        settle(m_data.isValid() ? QStringLiteral("stale") : QStringLiteral("unavailable"), why);
    });
}

void OmnuvRead::forget()
{
    ++m_seq;
    m_path.clear();
    m_data = QVariant();
    m_readAt = QDateTime();
    m_problem.clear();
    if (m_state != QLatin1String("loading")) {
        m_state = QStringLiteral("loading");
    }
    emit changed();
}

void OmnuvRead::retry()
{
    if (!m_path.isEmpty()) {
        read(m_path);
    }
}

void OmnuvRead::settle(const QString& state, const QString& problem)
{
    // Logged on a change of state only. Every minute's identical answer is
    // not news, and a log full of it hides the one line that is.
    if (state != m_state || problem != m_problem) {
        if (problem.isEmpty()) {
            qInfo().noquote() << "omnuv: estate" << m_name << state;
        }
        else {
            qWarning().noquote() << "omnuv: estate" << m_name << state << "-" << problem;
        }
    }
    m_state = state;
    m_problem = problem;
    emit changed();
}

OmnuvEstate::OmnuvEstate(OmnuvRead::Get get, QObject* parent)
    : QObject(parent),
      m_members(new OmnuvRead(QStringLiteral("members"), get, this)),
      m_parked(new OmnuvRead(QStringLiteral("parked"), get, this)),
      m_networks(new OmnuvRead(QStringLiteral("networks"), get, this)),
      m_devices(new OmnuvRead(QStringLiteral("devices"), get, this)),
      m_endpoints(new OmnuvRead(QStringLiteral("endpoints"), get, this)),
      m_keys(new OmnuvRead(QStringLiteral("keys"), get, this)),
      m_usage(new OmnuvRead(QStringLiteral("usage"), get, this)),
      m_history(new OmnuvRead(QStringLiteral("history"), get, this))
{
    // Devices hang off the project's network, so they are asked about once
    // the network is known — and asked again only if it is a different one.
    connect(m_networks, &OmnuvRead::landed, this, [this]() {
        const QVariantList nets = m_networks->data().toList();
        if (nets.isEmpty()) {
            m_devices->forget();
            return;
        }
        const QString id = nets.first().toMap().value(QStringLiteral("id")).toString();
        m_devices->read(QStringLiteral("/v1/networks/%1/devices").arg(id));
    });
}

void OmnuvEstate::setIdentity(const QString& organizationName, const QString& role)
{
    const bool owner = role == QLatin1String("owner");
    if (organizationName == m_organizationName && owner == m_owner) {
        return;
    }
    m_organizationName = organizationName;
    m_owner = owner;
    emit identityChanged();
}

void OmnuvEstate::setProject(const QString& projectId)
{
    if (projectId == m_project) {
        return;
    }
    m_project = projectId;
    for (OmnuvRead* r : { m_parked, m_networks, m_devices, m_endpoints, m_keys, m_usage, m_history }) {
        r->forget();
    }
    m_ticks = 0;
}

void OmnuvEstate::refresh(bool everything)
{
    if (everything) {
        m_ticks = 0;
    }
    if (m_ticks++ % 4 == 0) {
        readAll();
    }
    else {
        readFast();
    }
}

void OmnuvEstate::clear()
{
    m_project.clear();
    m_ticks = 0;
    for (OmnuvRead* r : { m_members, m_parked, m_networks, m_devices, m_endpoints, m_keys, m_usage, m_history }) {
        r->forget();
    }
    setIdentity(QString(), QString());
}

void OmnuvEstate::retryAll()
{
    for (OmnuvRead* r : { m_members, m_parked, m_networks, m_devices, m_endpoints, m_keys, m_usage, m_history }) {
        r->retry();
    }
}

void OmnuvEstate::readAll()
{
    readFast();
    readSlow();
}

void OmnuvEstate::readFast()
{
    // Nothing project-scoped is asked before the project is known: an
    // unscoped read answers for Core's default, which may not be the project
    // on screen.
    if (m_project.isEmpty()) {
        return;
    }
    const QString scope = QStringLiteral("?project=") + m_project;
    m_parked->read(QStringLiteral("/v1/parked") + scope);
    // Three, because that is what the window shows. Paged by `limit` alone:
    // Core computes the chain over the rows it returns, and a `from` would
    // leave the oldest of them without a predecessor.
    m_history->read(QStringLiteral("/v1/projects/%1/history?limit=3").arg(m_project));
}

void OmnuvEstate::readSlow()
{
    m_members->read(QStringLiteral("/v1/org/members"));
    if (m_project.isEmpty()) {
        return;
    }
    const QString scope = QStringLiteral("?project=") + m_project;
    // Devices are read again when this answer lands; see the constructor.
    m_networks->read(QStringLiteral("/v1/networks") + scope);
    m_endpoints->read(QStringLiteral("/v1/endpoints") + scope);
    m_keys->read(QStringLiteral("/v1/projects/%1/api-keys").arg(m_project));
    m_usage->read(QStringLiteral("/v1/usage?days=30&project=") + m_project);
}
