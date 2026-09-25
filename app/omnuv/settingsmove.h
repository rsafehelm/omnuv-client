// Omnuv: moving a person's settings to the application's own identity.
//
// The macOS client carried Moonlight's identity, com.moonlight-stream, until
// 25 September 2026: its bundle identifier, and, through
// QCoreApplication::setOrganizationDomain, the domain its settings live under
// (com.moonlight-stream.OmnuvClient). It is dev.omnuv.OmnuvClient now, beside
// the package (dev.omnuv.connect) and the tunnel (dev.omnuv.tunnel). Those
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
