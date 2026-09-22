#include "../credentials.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class CredentialsTest : public QObject {
    Q_OBJECT
private slots:
    void actualStoreRoundTripAndRepeatedClear() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("OMNUV_CREDENTIAL_TEST_DIR", directory.path().toUtf8());
        QVERIFY(OmnuvCredentials::clear());
        QVERIFY(OmnuvCredentials::store(QString::fromUtf8("fixture-\xc3\xa9-token")));
        QCOMPARE(OmnuvCredentials::load(), QString::fromUtf8("fixture-\xc3\xa9-token"));
#ifndef Q_OS_WIN
        QFile file(directory.filePath("token"));
        const auto permissions=file.permissions();
        QVERIFY(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
            | QFileDevice::ReadOther | QFileDevice::WriteOther)));
        QVERIFY(OmnuvCredentials::store("replacement"));
        QCOMPARE(OmnuvCredentials::load(), QString("replacement"));
#else
        QVERIFY(!OmnuvCredentials::store(QString(6000, QLatin1Char('x'))));
        QVERIFY(!OmnuvCredentials::load().isEmpty());
#endif
        QVERIFY(OmnuvCredentials::clear());
        QVERIFY(OmnuvCredentials::load().isEmpty());
        QVERIFY(OmnuvCredentials::clear());
    }
    void failedFileWriteAndDeleteAreReported() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        qputenv("OMNUV_CREDENTIAL_TEST_DIR", directory.path().toUtf8());
        QVERIFY(QDir().mkdir(directory.filePath("token")));
#ifndef Q_OS_WIN
        QVERIFY(!OmnuvCredentials::store("must-not-be-saved"));
#endif
        QVERIFY(!OmnuvCredentials::clear());
        QVERIFY(QDir(directory.filePath("token")).exists());
        QVERIFY(QDir().rmdir(directory.filePath("token")));
        QVERIFY(OmnuvCredentials::clear());
    }
};
QTEST_GUILESS_MAIN(CredentialsTest)
#include "credentials_test.moc"
