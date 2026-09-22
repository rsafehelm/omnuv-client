#include "credentials.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>
#ifndef Q_OS_WIN
#include <cerrno>
#include <sys/stat.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

// The name the credential appears under in Credential Manager. A person
// looking at that list should be able to tell what it is and delete it, so it
// says the product name rather than an opaque identifier.
#ifdef OMNUV_CREDENTIALS_TESTING
const wchar_t* credentialTarget()
{
    static const std::wstring name = (QStringLiteral("Omnuv Connect test ")
        + qEnvironmentVariable("OMNUV_CREDENTIAL_TEST_DIR")).toStdWString();
    return name.c_str();
}
#else
const wchar_t* credentialTarget() { return L"Omnuv Connect"; }
#endif

// Where the token used to live, and still does on platforms without an
// implementation here.
QString filePath()
{
#ifdef OMNUV_CREDENTIALS_TESTING
    return qEnvironmentVariable("OMNUV_CREDENTIAL_TEST_DIR") + QStringLiteral("/token");
#elif defined(Q_OS_WIN)
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
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = token.toUtf8();
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
            || file.write(bytes) != bytes.size() || !file.flush()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

bool removeFile()
{
    const QString path = filePath();
    if (QFile::remove(path)) return true;
#ifdef Q_OS_WIN
    if (GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16())) == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
    }
#else
    struct stat status;
    if (::lstat(QFile::encodeName(path).constData(), &status) < 0 && errno == ENOENT) return true;
#endif
    qWarning("Omnuv: could not remove the saved credential file");
    return false;
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
    if (CredReadW(credentialTarget(), CRED_TYPE_GENERIC, 0, &cred) && cred != nullptr) {
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
            if (removeFile()) qInfo("Omnuv: moved the saved token into Credential Manager");
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
    cred.TargetName = const_cast<wchar_t*>(credentialTarget());
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

bool OmnuvCredentials::clear()
{
    bool cleared = true;
#ifdef Q_OS_WIN
    if (!CredDeleteW(credentialTarget(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        qWarning("Omnuv: could not delete the token from Credential Manager (%lu)", GetLastError());
        cleared = false;
    }
#endif
    // Try both stores even if one fails. Absence is success; unreadability is not.
    const bool legacyCleared = removeFile();
    return cleared && legacyCleared;
}
