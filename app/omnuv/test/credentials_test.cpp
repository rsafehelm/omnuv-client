#include "../credentials.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

namespace {
const QString kProd = QStringLiteral("https://api.omnuv.com");
const QString kTest = QStringLiteral("https://api.test.omnuv.com");

QString slotFile(const QTemporaryDir& d, const QString& origin)
{
    return d.filePath(QStringLiteral("tokens/") + QString::fromLatin1(
        QCryptographicHash::hash(origin.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)));
}

bool writeLegacy(const QTemporaryDir& d, const QByteArray& token)
{
    QFile f(d.filePath("token"));
    return f.open(QIODevice::WriteOnly) && f.write(token) == token.size();
}
}

class CredentialsTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        m_directory.reset(new QTemporaryDir);
        QVERIFY(m_directory->isValid());
        qputenv("OMNUV_CREDENTIAL_TEST_DIR", m_directory->path().toUtf8());
    }

    // The origin is the key, so two spellings of one Core must be one key and
    // two Cores must never be.
    void anOriginIsNormalised() {
        QCOMPARE(OmnuvCredentials::origin("HTTPS://API.Omnuv.com:443/v1/me?x=1#y"), kProd);
        QCOMPARE(OmnuvCredentials::origin("https://api.omnuv.com/"), kProd);
        QCOMPARE(OmnuvCredentials::origin("https://user:pw@api.omnuv.com"), kProd);
        QCOMPARE(OmnuvCredentials::origin("http://127.0.0.1:8090/"), QString("http://127.0.0.1:8090"));
        QVERIFY(OmnuvCredentials::origin("https://api.omnuv.com") != OmnuvCredentials::origin("https://api.test.omnuv.com"));
        QVERIFY(OmnuvCredentials::origin("ftp://api.omnuv.com").isEmpty());
        QVERIFY(OmnuvCredentials::origin("not a url").isEmpty());
        QVERIFY(OmnuvCredentials::origin(QString()).isEmpty());
    }

    // H2: a token goes over https, or in the clear only to this machine.
    void onlyASecureCoreGetsAToken() {
        QVERIFY(OmnuvCredentials::secureCore("https://api.omnuv.com"));
        QVERIFY(OmnuvCredentials::secureCore("HTTPS://api.test.omnuv.com:8443/"));
        QVERIFY(OmnuvCredentials::secureCore("http://127.0.0.1:8090"));
        QVERIFY(OmnuvCredentials::secureCore("http://127.1.2.3"));
        QVERIFY(OmnuvCredentials::secureCore("http://[::1]:8090"));
        // The nearest things that must be refused.
        QVERIFY(!OmnuvCredentials::secureCore("http://api.omnuv.com"));
        QVERIFY(!OmnuvCredentials::secureCore("http://localhost:8090"));
        QVERIFY(!OmnuvCredentials::secureCore("http://127.0.0.1.attacker.example"));
        QVERIFY(!OmnuvCredentials::secureCore("http://128.0.0.1"));
        QVERIFY(!OmnuvCredentials::secureCore("http://127.0.0.256"));
        QVERIFY(!OmnuvCredentials::secureCore("http://192.168.100.85"));
        QVERIFY(!OmnuvCredentials::secureCore("ftp://api.omnuv.com"));
        QVERIFY(!OmnuvCredentials::secureCore("https://"));
        QVERIFY(!OmnuvCredentials::secureCore(QString()));
    }

    void actualStoreRoundTripAndRepeatedClear() {
        QVERIFY(OmnuvCredentials::clear(kProd));
        QVERIFY(OmnuvCredentials::store(kProd, QString::fromUtf8("fixture-\xc3\xa9-token")));
        QCOMPARE(OmnuvCredentials::load(kProd), QString::fromUtf8("fixture-\xc3\xa9-token"));
#ifndef Q_OS_WIN
        QFile file(slotFile(*m_directory, kProd));
        QVERIFY(file.exists());
        const auto permissions=file.permissions();
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther)));
        QVERIFY(OmnuvCredentials::store(kProd, "replacement"));
        QCOMPARE(OmnuvCredentials::load(kProd), QString("replacement"));
#else
        QVERIFY(!OmnuvCredentials::store(kProd, QString(6000, QLatin1Char('x'))));
        QVERIFY(!OmnuvCredentials::load(kProd).isEmpty());
#endif
        QVERIFY(OmnuvCredentials::clear(kProd));
        QVERIFY(OmnuvCredentials::load(kProd).isEmpty());
        QVERIFY(OmnuvCredentials::clear(kProd));
    }

    // **The point of the change.** Signed in to both, each keeps its own; one
    // cleared leaves the other; neither is read for the other's origin.
    void twoCoresKeepTheirOwnSignIn() {
        QVERIFY(OmnuvCredentials::store(kProd, "prod-token"));
        QVERIFY(OmnuvCredentials::store(kTest, "test-token"));
        QCOMPARE(OmnuvCredentials::load(kProd), QString("prod-token"));
        QCOMPARE(OmnuvCredentials::load(kTest), QString("test-token"));
        QVERIFY(OmnuvCredentials::clear(kTest));
        QVERIFY(OmnuvCredentials::load(kTest).isEmpty());
        QCOMPARE(OmnuvCredentials::load(kProd), QString("prod-token"));
    }

    // An origin that is not one stores nothing and reads nothing, rather than
    // landing in a slot every bad address would share.
    void anEmptyOriginHasNoSlot() {
        QVERIFY(!OmnuvCredentials::store(QString(), "nowhere"));
        QVERIFY(OmnuvCredentials::load(QString()).isEmpty());
        QVERIFY(OmnuvCredentials::clear(QString()));
    }

    // On Windows the old single slot was the app's alone, so it moves into
    // the origin it was saved beside, once, and is gone from the old place.
    // Elsewhere `omnuv-connect sign-in <core>` wrote the same file for any
    // Core, so nothing proves its origin: it is removed, never sent (H16).
    void theOldSlotMovesToItsOwnCoreOnlyWhereThatIsProvable() {
        QVERIFY(writeLegacy(*m_directory, "legacy-token"));
#ifdef Q_OS_WIN
        QCOMPARE(OmnuvCredentials::load(kProd, kProd), QString("legacy-token"));
        QCOMPARE(OmnuvCredentials::load(kProd), QString("legacy-token"));
#else
        QVERIFY(OmnuvCredentials::load(kProd, kProd).isEmpty());
        QVERIFY(OmnuvCredentials::load(kProd).isEmpty());
        QVERIFY(!QFile::exists(slotFile(*m_directory, kProd)));
#endif
        QVERIFY(!QFile::exists(m_directory->filePath("token")));
    }

    // H16: signing out removes the old slot too, when it was this Core's or
    // nobody's; one saved beside another address is that address's.
    void signingOutTakesTheOldSlotUnlessItIsAnotherCores() {
        QVERIFY(writeLegacy(*m_directory, "legacy-token"));
        QVERIFY(OmnuvCredentials::clear(kTest, kProd));
        QVERIFY(QFile::exists(m_directory->filePath("token")));
        QVERIFY(OmnuvCredentials::clear(kProd, kProd));
        QVERIFY(!QFile::exists(m_directory->filePath("token")));
        QVERIFY(writeLegacy(*m_directory, "orphan-token"));
        QVERIFY(OmnuvCredentials::clear(kTest));
        QVERIFY(!QFile::exists(m_directory->filePath("token")));
    }

    // **And never into another.** The token was saved beside production's
    // address; asked for the test Core, it stays where it is and the test Core
    // gets nothing. This is the B10 failure: production's token sent to the
    // mirror.
    void theOldSlotIsNeverGuessedIntoAnotherCore() {
        QVERIFY(writeLegacy(*m_directory, "legacy-token"));
        QVERIFY(OmnuvCredentials::load(kTest, kProd).isEmpty());
        QVERIFY(OmnuvCredentials::load(kTest).isEmpty());
        QVERIFY(QFile::exists(m_directory->filePath("token")));
    }

    void failedFileWriteAndDeleteAreReported() {
#ifndef Q_OS_WIN
        const QString slot = slotFile(*m_directory, kProd);
        QVERIFY(QDir().mkpath(slot));
        QVERIFY(!OmnuvCredentials::store(kProd, "must-not-be-saved"));
        QVERIFY(!OmnuvCredentials::clear(kProd));
        QVERIFY(QDir(slot).exists());
        QVERIFY(QDir().rmdir(slot));
        QVERIFY(OmnuvCredentials::clear(kProd));
#endif
    }

private:
    QScopedPointer<QTemporaryDir> m_directory;
};
QTEST_GUILESS_MAIN(CredentialsTest)
#include "credentials_test.moc"
