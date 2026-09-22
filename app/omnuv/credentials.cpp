#include "credentials.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QUrl>
#ifndef Q_OS_WIN
#include <cerrno>
#include <sys/stat.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

#ifdef Q_OS_WIN
// The name the credential appears under in Credential Manager. A person
// looking at that list should be able to tell what it is and delete it, so it
// says the product name, then the Core it signs in to.
#ifdef OMNUV_CREDENTIALS_TESTING
QString targetBase()
{
    return QStringLiteral("Omnuv Connect test ") + qEnvironmentVariable("OMNUV_CREDENTIAL_TEST_DIR");
}
#else
QString targetBase() { return QStringLiteral("Omnuv Connect"); }
#endif

// The old single slot, and a per-origin one. `legacy` is the empty origin.
std::wstring target(const QString& origin)
{
    return (origin.isEmpty() ? targetBase() : targetBase() + QLatin1Char('/') + origin).toStdWString();
}
#endif

QString configDir()
{
#ifdef OMNUV_CREDENTIALS_TESTING
    return qEnvironmentVariable("OMNUV_CREDENTIAL_TEST_DIR");
#elif defined(Q_OS_WIN)
    return QDir::homePath() + QStringLiteral("/AppData/Roaming/Omnuv");
#else
    return QDir::homePath() + QStringLiteral("/.config/omnuv");
#endif
}

// Where the token lives on platforms without a store implementation here, and
// where every version before 22 September kept its single one (the empty
// origin). A hash rather than the origin itself, because an origin carries
// characters a file name may not.
QString filePath(const QString& origin)
{
    if (origin.isEmpty()) return configDir() + QStringLiteral("/token");
    const QByteArray digest = QCryptographicHash::hash(origin.toUtf8(), QCryptographicHash::Sha256).toHex();
    return configDir() + QStringLiteral("/tokens/") + QString::fromLatin1(digest.left(16));
}

QString readFile(const QString& origin)
{
    QFile f(filePath(origin));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool writeFile(const QString& origin, const QString& token)
{
    const QString path = filePath(origin);
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

bool removeFile(const QString& origin)
{
    const QString path = filePath(origin);
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

#ifdef Q_OS_WIN
QString readStore(const QString& origin)
{
    PCREDENTIALW cred = nullptr;
    const std::wstring name = target(origin);
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &cred) || cred == nullptr) return QString();
    const QString token = QString::fromUtf8(
        reinterpret_cast<const char*>(cred->CredentialBlob),
        static_cast<int>(cred->CredentialBlobSize)).trimmed();
    CredFree(cred);
    return token;
}

bool deleteStore(const QString& origin)
{
    const std::wstring name = target(origin);
    if (!CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND) {
        qWarning("Omnuv: could not delete the token from Credential Manager (%lu)", GetLastError());
        return false;
    }
    return true;
}
#endif

// The token in the old single slot, wherever this platform kept it, and the
// removal of every copy of it.
QString readLegacy()
{
#ifdef Q_OS_WIN
    const QString fromStore = readStore(QString());
    if (!fromStore.isEmpty()) return fromStore;
#endif
    return readFile(QString());
}

bool clearLegacy()
{
    bool cleared = true;
#ifdef Q_OS_WIN
    cleared = deleteStore(QString());
#endif
    return removeFile(QString()) && cleared;
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

QString OmnuvCredentials::origin(const QString& coreUrl)
{
    QUrl core(coreUrl.trimmed());
    const QString scheme = core.scheme().toLower();
    if ((scheme != QLatin1String("https") && scheme != QLatin1String("http")) || core.host().isEmpty()) {
        return QString();
    }
    core.setScheme(scheme);
    core.setHost(core.host().toLower());
    if ((scheme == QLatin1String("https") && core.port() == 443) || (scheme == QLatin1String("http") && core.port() == 80)) {
        core.setPort(-1);
    }
    core.setPath(QString());
    core.setQuery(QString());
    core.setFragment(QString());
    return core.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash);
}

bool OmnuvCredentials::secureCore(const QString& coreUrl)
{
    const QUrl core(coreUrl.trimmed());
    const QString scheme = core.scheme().toLower();
    const QString host = core.host().toLower();
    if (host.isEmpty()) return false;
    if (scheme == QLatin1String("https")) return true;
    if (scheme != QLatin1String("http")) return false;
    // Loopback only, by address rather than by name: `localhost` is a name a
    // resolver could answer otherwise, and 127.0.0.0/8 and ::1 are not. Parsed
    // here rather than with QHostAddress, which is QtNetwork: the credentials
    // checks on the Mac and Windows rigs link QtCore alone.
    if (host == QLatin1String("::1")) return true;
    const QStringList octets = host.split(QLatin1Char('.'));
    if (octets.size() != 4 || octets.first() != QLatin1String("127")) return false;
    for (const QString& octet : octets) {
        bool ok = false;
        const int value = octet.toInt(&ok);
        if (!ok || value < 0 || value > 255 || octet.isEmpty() || octet.size() > 3) return false;
    }
    return true;
}

QString OmnuvCredentials::load(const QString& origin, const QString& legacyOwner)
{
    if (origin.isEmpty()) return QString();
#ifdef Q_OS_WIN
    const QString fromStore = readStore(origin);
    if (!fromStore.isEmpty()) return fromStore;
    // A file an earlier Windows build left for this origin cannot exist: the
    // per-origin slot is newer than the file fallback there.
#else
    const QString own = readFile(origin);
    if (!own.isEmpty()) return own;
#endif

    // Nothing for this origin. The old single slot is this origin's only if
    // it was saved beside this address; anything else stays where it is.
    if (legacyOwner.isEmpty() || legacyOwner != origin) return QString();
    const QString legacy = readLegacy();
    if (legacy.isEmpty()) return QString();
    if (store(origin, legacy)) {
        if (clearLegacy()) qInfo("Omnuv: moved the saved token into its Core's own slot");
    }
    return legacy;
}

bool OmnuvCredentials::store(const QString& origin, const QString& token)
{
    if (origin.isEmpty()) return false;
#ifdef Q_OS_WIN
    const QByteArray utf8 = token.toUtf8();
    const std::wstring name = target(origin);

    CREDENTIALW cred;
    ZeroMemory(&cred, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<wchar_t*>(name.c_str());
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
    return writeFile(origin, token);
#endif
}

bool OmnuvCredentials::clear(const QString& origin)
{
    if (origin.isEmpty()) return true;
    bool cleared = true;
#ifdef Q_OS_WIN
    cleared = deleteStore(origin);
#endif
    // Try both even if one fails. Absence is success; unreadability is not.
    const bool fileCleared = removeFile(origin);
    return cleared && fileCleared;
}
