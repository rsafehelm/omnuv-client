// Omnuv: the machines a person rents, as a list QML can show.
//
// The fields are the ones the buyer API already returns. Nothing is derived
// here that Core could decide instead — this model displays, it does not
// choose. That split is what keeps a published client from becoming a second
// place where marketplace behaviour lives.

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>

class QJsonArray;

struct Machine
{
    QString id;
    QString name;
    QString region;
    QString status;
    // The application the machine streams, when it streams one. Empty for an
    // ordinary machine, which is how the two are told apart.
    QString streamApp;
    QString defaultUser;
    QString summary;   // "8 vCPU · 16 GiB · RTX 3090"

    // The name it answers to on the project network, as Core gives it. The
    // only address this application ever uses: a private IP would work today
    // and stop working the moment a machine moves.
    //
    // **Core's, not ours.** This used to be `name.toLower() + ".internal"`,
    // which is the client deciding a naming rule that belongs to the side that
    // allocates the address. It happened to agree with Core, and that is the
    // problem: the day Core prefixes a project, or lowercases differently, or
    // gives a machine a second name, the client would go on confidently
    // building an address nothing answers to. Empty until an address is
    // assigned, which is also when there is nothing to connect to.
    QString host;

    bool streamed() const { return !streamApp.isEmpty(); }
};

class MachineModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        RegionRole,
        StatusRole,
        StreamAppRole,
        StreamedRole,
        HostRole,
        UserRole,
        SummaryRole,
        // True when this machine can be connected to right now. A stopped or
        // starting machine is shown, and shown as unreachable, rather than
        // hidden — somebody who cannot find their machine assumes it is lost.
        ReadyRole,
    };

    explicit MachineModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replaces the list with what the API just returned.
    void replace(const QJsonArray& machines);
    void clear();

    Q_INVOKABLE QString nameAt(int row) const;
    Q_INVOKABLE QString hostAt(int row) const;
    Q_INVOKABLE QString userAt(int row) const;
    Q_INVOKABLE bool streamedAt(int row) const;
    Q_INVOKABLE QString streamAppAt(int row) const;

signals:
    // A machine has finished starting. **The only thing here worth
    // interrupting somebody for**: they asked for it, they have been
    // waiting, and now they can use it. A failed poll is not — it means
    // nothing to a person and it recovers by itself.
    void machineBecameReady(const QString& name);
    void countChanged();

private:
    QList<Machine> m_machines;

    // Status per machine id as of the last refresh, so a transition can be
    // told from a steady state. Empty until the first load, which is what
    // stops every machine announcing itself when the application starts.
    QHash<QString, QString> m_lastStatus;
    bool m_loadedOnce = false;
};
