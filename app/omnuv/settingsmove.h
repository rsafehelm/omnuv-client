// Omnuv: moving a person's settings to the application's own identity.
//
// The macOS client carried Moonlight's identity, com.moonlight-stream, until
// 25 September 2026: its bundle identifier, and, through
// QCoreApplication::setOrganizationDomain, the domain its settings live under
// (com.moonlight-stream.OmnuvClient). The bundle is dev.omnuv.OmnuvClient
// now, beside the package (dev.omnuv.connect) and the tunnel
// (dev.omnuv.tunnel), and the settings domain com.omnuv.OmnuvClient, from
// Omnuv's own domain: Qt turns a domain whose suffix it does not know, such as
// omnuv.dev, into com.omnuv-dev. Those
// settings hold the paired machines and this device's pairing identity, the
// saved deployment and the chosen project, so starting from empty would ask a
// person to pair every machine again.
//
// On Windows and Linux the location follows the organization *name*, which
// did not change, so this is only called on macOS; it takes two QSettings so a
// test can hand it two files on any platform.
#pragma once

#include <QSettings>
#include <QString>

// Copies every key of `from` into `to`, once: only when `to` holds nothing and
// `from` holds something. The old copy is kept, so an older client still finds
// its own. Answers whether anything was copied.
inline bool omnuvMoveSettingsOnce(QSettings& from, QSettings& to)
{
    // **Each side's own keys only.** A QSettings also answers from its
    // fallbacks, and on macOS those include the system-wide global domain, so
    // a brand-new domain never looked empty and nothing was moved (rig 9101,
    // 25 September 2026). Restored after, for whoever holds these objects.
    const bool fromFalls = from.fallbacksEnabled(), toFalls = to.fallbacksEnabled();
    from.setFallbacksEnabled(false);
    to.setFallbacksEnabled(false);
    struct Restore {
        QSettings& a; QSettings& b; bool fa, fb;
        ~Restore() { a.setFallbacksEnabled(fa); b.setFallbacksEnabled(fb); }
    } restore{from, to, fromFalls, toFalls};
    if (!to.allKeys().isEmpty() || from.allKeys().isEmpty()) {
        return false;
    }
    const QStringList keys = from.allKeys();
    for (const QString& key : keys) {
        to.setValue(key, from.value(key));
    }
    to.setValue(QStringLiteral("omnuv/settingsMovedFrom"), from.fileName());
    to.sync();
    return to.status() == QSettings::NoError;
}
