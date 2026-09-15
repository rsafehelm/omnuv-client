#include "credentials.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

// The name the credential appears under in Credential Manager. A person
// looking at that list should be able to tell what it is and delete it, so it
// says the product name rather than an opaque identifier.
const wchar_t* kTargetName = L"Omnuv Connect";

// Where the token used to live, and still does on platforms without an
// implementation here.
QString filePath()
{
#ifdef Q_OS_WIN
    return QDir::homePath() + QStringLiteral("/AppData/Roaming/Omnuv/token");
#else
    return QDir::homePath() + QStringLiteral("/.config/omnuv/token");
#endif
}

QString readFile()
{
    QFile f(filePath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool writeFile(const QString& token)
{
    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    // Before anything is written, so the token is never briefly world-readable.
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(token.toUtf8());
    return true;
}

} // namespace

bool OmnuvCredentials::usingPlatformStore()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

QString OmnuvCredentials::load()
{
#ifdef Q_OS_WIN
    PCREDENTIALW cred = nullptr;
    if (CredReadW(kTargetName, CRED_TYPE_GENERIC, 0, &cred) && cred != nullptr) {
        const QString token = QString::fromUtf8(
            reinterpret_cast<const char*>(cred->CredentialBlob),
            static_cast<int>(cred->CredentialBlobSize)).trimmed();
        CredFree(cred);
        if (!token.isEmpty()) {
            return token;
        }
    }

    // Nothing in the store. An older version of this program may have left a
    // token in the file — move it across and remove the file, because a
    // plaintext copy surviving the upgrade would give us the risk of both
    // designs and the benefit of neither.
    const QString fromFile = readFile();
    if (!fromFile.isEmpty()) {
        if (store(fromFile)) {
            QFile::remove(filePath());
            qInfo("Omnuv: moved the saved token into Credential Manager");
        }
        return fromFile;
    }
    return QString();
#else
    return readFile();
#endif
}

bool OmnuvCredentials::store(const QString& token)
{
#ifdef Q_OS_WIN
    const QByteArray utf8 = token.toUtf8();

    CREDENTIALW cred;
    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(kTargetName);
    cred.CredentialBlobSize = static_cast<DWORD>(utf8.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(utf8.constData()));
    // Survives a reboot, which is the entire point for a program that starts
    // itself at login. LOCAL_MACHINE would not roam and SESSION would not
    // survive, and both would silently sign the person out.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    if (!CredWriteW(&cred, 0)) {
        qWarning("Omnuv: could not write the token to Credential Manager (%lu)", GetLastError());
        return false;
    }
    return true;
#else
    return writeFile(token);
#endif
}

void OmnuvCredentials::clear()
{
#ifdef Q_OS_WIN
    CredDeleteW(kTargetName, CRED_TYPE_GENERIC, 0);
#endif
    // The file too, on every platform: signing out must not leave a token
    // behind anywhere, including one an older version wrote.
    QFile::remove(filePath());
}
