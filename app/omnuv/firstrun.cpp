#include "firstrun.h"

#include <QGuiApplication>
#include <QScreen>
#include <QDebug>
#include <QSettings>

// **A first stream at this display's size, not upstream's 1280x720** (the
// operator's Windows client on a 1440p screen, 30 September 2026: text was
// unreadable until Stream settings were opened). The largest standard size
// that fits the primary screen in physical pixels. Written as the saved
// choice only when there is none, before StreamingPreferences reads it, so
// upstream's preferences code and its bitrate default (which follows the
// resolution) stay as they are. A person's own choice is never touched.
// **V-Sync off unless somebody chose it** (the operator, 2 October 2026).
// With it on, a frame waits for the display's next refresh, and a stream rate
// that does not divide the refresh drops frames unevenly: 165 fps on a 480 Hz
// monitor rendered 82 and dropped 37%. Off, each frame is shown as it is
// decoded, which tears a little and drops nothing; that was the operator's own
// fix. Same rule as the resolution below: written only when there is no saved
// choice, before upstream's preferences read it (SER_VSYNC is "vsync").
static void omnuvApplyFirstRunVsync(QSettings& settings)
{
    if (settings.contains(QStringLiteral("vsync"))) return;
    settings.setValue(QStringLiteral("vsync"), false);
    qInfo().noquote() << "omnuv: first run: V-Sync off";
}

// **Connection quality warnings off unless somebody chose them** (the
// operator, 5 October 2026). Upstream's `connectionWarnings` gates the
// in-stream quality mark and its waiting state; the log line is written
// either way, which is what the harness reads. Same rule as V-Sync: only
// when there is no saved choice (SER_CONNWARNINGS is "connwarnings").
static void omnuvApplyFirstRunWarnings(QSettings& settings)
{
    if (settings.contains(QStringLiteral("connwarnings"))) return;
    settings.setValue(QStringLiteral("connwarnings"), false);
    qInfo().noquote() << "omnuv: first run: connection quality warnings off";
}

void omnuvApplyFirstRunResolution()
{
    QSettings settings;
    omnuvApplyFirstRunVsync(settings);
    omnuvApplyFirstRunWarnings(settings);
    if (settings.contains(QStringLiteral("width")) || settings.contains(QStringLiteral("height"))) return;
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) return;
    const QSize native = screen->size() * screen->devicePixelRatio();
    static const QSize standard[] = { {3840, 2160}, {2560, 1440}, {1920, 1080} };
    for (const QSize& s : standard) {
        if (s.width() <= native.width() && s.height() <= native.height()) {
            settings.setValue(QStringLiteral("width"), s.width());
            settings.setValue(QStringLiteral("height"), s.height());
            qInfo().noquote() << "omnuv: first run: streaming at" << s.width() << "x" << s.height()
                              << "on a" << native.width() << "x" << native.height() << "display";
            return;
        }
    }
}
