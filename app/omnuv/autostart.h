#pragma once

#include <QObject>

// Whether this program starts when the person logs in.
//
// **The whole point of a widget.** Everything else Omnuv Connect does already
// worked before residency; what was missing is that somebody had to open it
// first. A program you must remember to launch is not a widget, it is an
// application — and the machine list it shows is stale the moment you close it.
//
// Per platform, because there is no portable answer:
//
//   Windows  HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//   Linux    ~/.config/autostart/omnuv-connect.desktop
//   macOS    SMAppService / a LaunchAgent plist  (not implemented yet)
//
// **`HKCU`, never `HKLM`, and never a service or a scheduled task.** Each of
// those is a way of being harder to remove than to install, which is the
// defining habit of software people resent. A per-user Run key is what the
// person's own Settings app lists and can switch off without us.
class OmnuvAutostart : public QObject
{
    Q_OBJECT

    // Reading it asks the operating system rather than remembering what we
    // last wrote: a person may have turned it off in Settings, or another
    // install may have changed it, and a checkbox that reports our intention
    // instead of the machine's state is a checkbox that lies.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)

    // False where we have no implementation, so a UI can say so rather than
    // offering a switch that does nothing.
    Q_PROPERTY(bool supported READ supported CONSTANT)

public:
    explicit OmnuvAutostart(QObject* parent = nullptr);

    bool enabled() const;
    void setEnabled(bool on);
    bool supported() const;

private:
    const bool m_fixture = qEnvironmentVariableIsSet("OMNUV_FIXTURE_URL");

signals:
    void changed();
};
