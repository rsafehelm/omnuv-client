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
    QJsonObject records(bool* ok = nullptr) const {
        if (m_memory) { if (ok) *ok = true; return m_records; }
        auto settings = open();
        settings->sync();
        const auto raw = settings->value("omnuv/enrollments").toByteArray();
        QJsonParseError parsed;
        const auto document = QJsonDocument::fromJson(raw, &parsed);
        const bool valid = settings->status() == QSettings::NoError
            && (raw.isEmpty() || (parsed.error == QJsonParseError::NoError && document.isObject()));
        if (ok) *ok = valid;
        return document.object();
    }
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
    bool change(const QString& id, QJsonObject record, bool reserve,
                const QJsonObject& scope, QJsonObject* result) {
        const auto lockPath = m_path.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/enrollment-journal.lock"
            : m_path + ".journal-lock";
        std::unique_ptr<QLockFile> lock;
        if (!m_memory) {
            if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) return false;
            lock.reset(new QLockFile(lockPath));
            if (!lock->tryLock(0)) return false;
        }
        bool valid;
        auto values = records(&valid);
        if (!valid) return false; // a damaged recovery ledger must not be discarded
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
            if (settings->status() != QSettings::NoError) return false;
        }
        m_records = values;
        if (result) *result = record;
        return true;
    }
    bool m_memory;
    QString m_path;
    QJsonObject m_records;
};
