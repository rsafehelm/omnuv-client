#include "machinemodel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>

// One RFC 3339 timestamp from Core, as a moment.
//
// The fractional seconds are removed before parsing rather than trusted to a
// parser: Core formats with the `time` crate's rfc3339, which emits whatever
// subsecond precision the column holds — microseconds, from Postgres — and
// how many digits `Qt::ISODate` tolerates is not a thing to discover on a
// buyer's screen. Nothing on a card is measured finer than a second, so the
// digits are dropped rather than rounded.
//
// An absent or unparseable value returns an invalid QDateTime, which the model
// reports as "no observation" rather than as a date. That distinction is the
// whole point of the field.
static QDateTime moment(const QJsonValue& value)
{
    const QString text = value.toString();
    if (text.isEmpty()) {
        return QDateTime();
    }
    static const QRegularExpression fraction(QStringLiteral("\\.\\d+"));
    return QDateTime::fromString(QString(text).remove(fraction), Qt::ISODate);
}

MachineModel::MachineModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MachineModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_machines.count();
}

QVariant MachineModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_machines.count()) {
        return QVariant();
    }

    const Machine& m = m_machines.at(index.row());
    switch (role) {
    case NameRole:      return m.name;
    case RegionRole:    return m.region;
    case StatusRole:    return m.status;
    case StreamAppRole: return m.streamApp;
    case StreamedRole:  return m.streamed();
    case HostRole:      return m.host;
    case UserRole:      return m.defaultUser;
    case SummaryRole:   return m.summary;
    case PrivateIpRole: return m.privateIp;
    case LastErrorRole: return m.lastError;
    case OperatingRole: return m.operating;
    case OperationSinceRole:   return m.operationSince;
    case OperationAttemptRole: return m.operationAttempt;
    case WaitingOnRole:        return m.waitingOn;
    case ObservedRole:            return m.observed;
    case ObservedAtRole:          return m.observedAt;
    case ObservationCompleteRole: return m.observationComplete;
    // Running *and* reachable. A machine Core has not yet given a private name
    // has no address to connect to, and a Connect button that opens nothing is
    // worse than one that is plainly disabled.
    case ReadyRole:     return m.status == QStringLiteral("Running") && !m.host.isEmpty();
    default:            return QVariant();
    }
}

QHash<int, QByteArray> MachineModel::roleNames() const
{
    return {
        { NameRole,      "name" },
        { RegionRole,    "region" },
        { StatusRole,    "status" },
        { StreamAppRole, "streamApp" },
        { StreamedRole,  "streamed" },
        { HostRole,      "host" },
        { UserRole,      "user" },
        { SummaryRole,   "summary" },
        { PrivateIpRole, "privateIp" },
        { LastErrorRole, "lastError" },
        { OperatingRole,            "operating" },
        { OperationSinceRole,       "since" },
        { OperationAttemptRole,     "attempt" },
        { WaitingOnRole,            "waitingOn" },
        { ObservedRole,             "observed" },
        { ObservedAtRole,           "observedAt" },
        { ObservationCompleteRole,  "observationComplete" },
        { ReadyRole,     "ready" },
    };
}

// The one place the API's shape is read. Everything above works on Machine.
//
// A full reset every refresh, rather than a diff. With a handful of machines
// nobody sees it; with a hundred it would throw away the selection and the
// scroll position every fifteen seconds, and then it is worth comparing rows.
void MachineModel::replace(const QJsonArray& machines)
{
    // Remember what each machine was, so a *transition* can be told from a
    // steady state. Announcing "gpu-1 is ready" every fifteen seconds for as
    // long as it stays ready is how people turn notifications off.
    QHash<QString, QString> previous = m_lastStatus;

    beginResetModel();
    m_machines.clear();

    for (const QJsonValue& value : machines) {
        const QJsonObject o = value.toObject();

        Machine m;
        m.id = o["id"].toString();
        m.name = o["name"].toString();
        m.region = o["region"].toString();
        m.status = o["status"].toString();
        m.streamApp = o["stream_app"].toString();
        m.defaultUser = o["default_user"].toString();
        m.host = o["private_name"].toString();
        m.privateIp = o["private_ip"].toString();
        m.lastError = o["last_error"].toString();

        // Both objects are omitted entirely when Core has nothing to say, so
        // the test is on the object and not on a field inside it. Core builds
        // `operation` all-or-nothing — id, action and since together or no
        // object — so one presence test is the whole guard.
        const QJsonObject operation = o["operation"].toObject();
        m.operating = !operation.isEmpty();
        m.operationSince = moment(operation["since"]);
        m.operationAttempt = operation["attempt"].toInt(1);
        m.waitingOn = operation["waiting_on"].toString();

        const QJsonObject observation = o["observation"].toObject();
        m.observed = !observation.isEmpty();
        m.observedAt = moment(observation["collected_at"]);
        m.observationComplete = observation["complete"].toBool();

        // Enough to tell two machines apart at a glance, in the units the
        // console uses. GiB rather than MiB: nobody rents 16384 of anything.
        QStringList parts;
        parts << QStringLiteral("%1 vCPU").arg(o["vcpus"].toInt());
        parts << QStringLiteral("%1 GiB").arg(o["memory_mib"].toInt() / 1024);
        const QJsonObject gpu = o["gpu"].toObject();
        if (!gpu.isEmpty()) {
            const int count = gpu["count"].toInt(1);
            parts << (count > 1 ? QStringLiteral("%1 × %2").arg(count).arg(gpu["model"].toString())
                                : gpu["model"].toString());
        }
        m.summary = parts.join(QStringLiteral(" · "));

        m_machines.append(m);
    }

    m_lastStatus.clear();
    QStringList becameReady;
    for (const Machine& m : m_machines) {
        m_lastStatus.insert(m.id, m.status);
        const QString was = previous.value(m.id);
        if (m.status == QStringLiteral("Running") && was != QStringLiteral("Running")) {
            becameReady.append(m.name);
        }
    }

    endResetModel();

    // Not on the first load: everything would look new, and a person opening
    // the application does not need to be told about machines they can already
    // see on the screen in front of them.
    if (m_loadedOnce) {
        for (const QString& name : becameReady) {
            emit machineBecameReady(name);
        }
    }
    m_loadedOnce = true;
    emit countChanged();
}

void MachineModel::clear()
{
    if (m_machines.isEmpty()) {
        return;
    }

    beginResetModel();
    m_machines.clear();
    endResetModel();
    emit countChanged();
}

QString MachineModel::idAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).id : QString();
}

QString MachineModel::nameAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).name : QString();
}

QString MachineModel::userAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).defaultUser : QString();
}

QString MachineModel::hostAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).host : QString();
}

bool MachineModel::streamedAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) && m_machines.at(row).streamed();
}

QString MachineModel::streamAppAt(int row) const
{
    return (row >= 0 && row < m_machines.count()) ? m_machines.at(row).streamApp : QString();
}
