#include "machinemodel.h"

#include <QJsonArray>
#include <QJsonObject>

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
