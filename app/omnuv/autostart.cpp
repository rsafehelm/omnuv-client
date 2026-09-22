#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <QSettings>
namespace {
// The name the entry appears under in Settings > Apps > Startup, and the key
// we look for. Stable: renaming it would strand the old entry, which would
// then start a program the person thinks they disabled.
const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const char* kValueName = "Omnuv Connect";
}
#endif

#ifdef Q_OS_LINUX
namespace {
QString desktopFilePath()
{
    return QDir::homePath() + QStringLiteral("/.config/autostart/omnuv-connect.desktop");
}
}
#endif

OmnuvAutostart::OmnuvAutostart(QObject* parent) : QObject(parent)
{
}

bool OmnuvAutostart::supported() const
{
    if (m_fixture) return false;
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    // macOS needs SMAppService or a LaunchAgent plist and neither is written.
    // Saying so is better than a switch that silently does nothing.
    return false;
#endif
}

bool OmnuvAutostart::enabled() const
{
    if (m_fixture) return false;
#if defined(Q_OS_WIN)
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    return run.contains(QString::fromLatin1(kValueName));
#elif defined(Q_OS_LINUX)
    return QFile::exists(desktopFilePath());
#else
    return false;
#endif
}

void OmnuvAutostart::setEnabled(bool on)
{
    if (m_fixture) return;
    if (on == enabled()) {
        return;
    }

#if defined(Q_OS_WIN)
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (on) {
        // Quoted, and native separators. Without the quotes a path containing
        // a space — `C:\Program Files\...`, which is where this normally
        // lives — is read by Windows as a program name and arguments, and the
        // entry silently fails at every login.
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(QString::fromLatin1(kValueName), QStringLiteral("\"%1\"").arg(exe));
    } else {
        run.remove(QString::fromLatin1(kValueName));
    }
    run.sync();

#elif defined(Q_OS_LINUX)
    const QString path = desktopFilePath();
    if (on) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream out(&f);
            out << "[Desktop Entry]\n"
                << "Type=Application\n"
                << "Name=Omnuv Connect\n"
                << "Exec=" << QCoreApplication::applicationFilePath() << "\n"
                << "Terminal=false\n"
                << "X-GNOME-Autostart-enabled=true\n";
        }
    } else {
        QFile::remove(path);
    }
#else
    Q_UNUSED(on)
#endif

    emit changed();
}
