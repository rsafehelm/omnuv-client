#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <memory>

// Public recovery identifiers only. Setup keys and account tokens never enter
// this store. A synced reservation precedes the first request that can create.
class OmnuvEnrollmentJournal {
public:
    explicit OmnuvEnrollmentJournal(bool memory = false, const QString& path = {})
        : m_memory(memory), m_path(path) {}
    static QString key(const QJsonObject& scope) {
        auto publicScope = scope;
        publicScope.remove("device_id");
        return QString::fromLatin1(QCryptographicHash::hash(
            QJsonDocument(publicScope).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    }
    QJsonObject records(bool* ok = nullptr, QString* why = nullptr) const {
        if (m_memory) { if (ok) *ok = true; return m_records; }
        auto settings = open();
        settings->sync();
        const auto raw = settings->value("omnuv/enrollments").toByteArray();
        QJsonParseError parsed;
        const auto document = QJsonDocument::fromJson(raw, &parsed);
        const bool readable = settings->status() == QSettings::NoError;
        const bool valid = readable
            && (raw.isEmpty() || (parsed.error == QJsonParseError::NoError && document.isObject()));
        if (ok) *ok = valid;
        if (why && !valid) *why = (readable ? QStringLiteral("the saved enrollment record in %1 is damaged")
                                            : QStringLiteral("the settings in %1 cannot be read")).arg(place(*settings));
        return document.object();
    }
    // Why the last reserve or put failed, naming the file (5 October 2026).
    // The client said one sentence for all of them, and the operator's twice
    // was a settings file under Program Files: an installer had shipped
    // portable.dat, so the client kept its settings beside itself.
    QString error() const { return m_error; }
    // reserve is serialized across client processes: two windows reuse one ID.
    bool reserve(const QJsonObject& scope, QJsonObject* record) {
        return change(key(scope), {}, true, scope, record);
    }
    bool put(const QString& key, const QJsonObject& record) {
        return change(key, record, false, {}, nullptr);
    }
    bool remove(const QString& key) { return put(key, {}); }
private:
    std::unique_ptr<QSettings> open() const {
        return m_path.isEmpty() ? std::unique_ptr<QSettings>(new QSettings)
            : std::unique_ptr<QSettings>(new QSettings(m_path, QSettings::IniFormat));
    }
    static QString place(const QSettings& settings) { return QDir::toNativeSeparators(settings.fileName()); }
    static QString lockFailure(const QLockFile& lock, const QString& path) {
        const auto file = QDir::toNativeSeparators(path);
        if (lock.error() == QLockFile::LockFailedError) {
            qint64 pid = 0; QString host, app;
            return lock.getLockInfo(&pid, &host, &app)
                ? QStringLiteral("another Omnuv window (process %1) is saving an enrollment; quit it and retry").arg(pid)
                : QStringLiteral("another Omnuv window is saving an enrollment (%1); quit it and retry").arg(file);
        }
        return lock.error() == QLockFile::PermissionError
            ? QStringLiteral("the lock file %1 cannot be created: permission denied").arg(file)
            : QStringLiteral("the lock file %1 cannot be created").arg(file);
    }
    bool fail(const QString& why) { m_error = why; return false; }
    bool change(const QString& id, QJsonObject record, bool reserve,
                const QJsonObject& scope, QJsonObject* result) {
        const auto lockPath = m_path.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/enrollment-journal.lock"
            : m_path + ".journal-lock";
        m_error.clear();
        std::unique_ptr<QLockFile> lock;
        if (!m_memory) {
            const auto folder = QFileInfo(lockPath).absolutePath();
            if (!QDir().mkpath(folder))
                return fail(QStringLiteral("the folder %1 cannot be created").arg(QDir::toNativeSeparators(folder)));
            lock.reset(new QLockFile(lockPath));
            // **Waited for, briefly (24 September 2026).** `tryLock(0)` gave
            // up at once, so a second window reserving while the first held
            // the lock told its user the attempt "could not be saved". What
            // the lock guards is one settings write; two seconds is many of
            // them. A holder that died is noticed by QLockFile itself (the
            // process is gone) and does not cost the wait.
            if (!lock->tryLock(2000)) return fail(lockFailure(*lock, lockPath));
        }
        bool valid;
        QString why;
        auto values = records(&valid, &why);
        if (!valid) return fail(why); // a damaged recovery ledger must not be discarded
        if (reserve) {
            record = values.value(id).toObject();
            if (record.isEmpty() || record.value("cleanup_done").toBool()) {
                record = {{"membership", scope}};
                auto membership = scope;
                membership["device_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
                record["membership"] = membership;
            }
        }
        if (record.isEmpty()) values.remove(id); else values[id] = record;
        if (!m_memory) {
            auto settings = open();
            settings->setValue("omnuv/enrollments", QJsonDocument(values).toJson(QJsonDocument::Compact));
            settings->sync();
            if (settings->status() != QSettings::NoError)
                return fail(QStringLiteral("the settings in %1 cannot be written").arg(place(*settings)));
        }
        m_records = values;
        if (result) *result = record;
        return true;
    }
    bool m_memory;
    QString m_path;
    QJsonObject m_records;
    QString m_error;
};
