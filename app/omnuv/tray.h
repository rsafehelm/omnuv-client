#pragma once

#include <QObject>
#include <QSystemTrayIcon>

class QMenu;
class QAction;
class OmnuvSession;

// The tray, which is the whole of being a widget.
//
// A widget is a program that was already running when you needed it.
// Everything else in this application already works; what was missing is that
// it had to be opened before it knew anything.
//
// **C++ and QSystemTrayIcon, not QML and Qt.labs.platform, and that was
// learned the expensive way.** The first version of this was a QML file
// importing `Qt.labs.platform`. It compiled, it linked, it deployed, and then
// every Windows build — CI's and the lab rig's alike — exited a second after
// launch with no window and no error on screen:
//
//     QQmlApplicationEngine failed to load component
//     qrc:/gui/main.qml: Type OmnuvTray unavailable
//     qrc:/omnuv/OmnuvTray.qml: module "Qt.labs.platform" is not installed
//
// The module was present in the Qt installation and absent from the deployed
// application, because `scripts/build-arch.bat` runs
// `windeployqt --qmldir app\gui` — it scans *that one directory* to decide
// which QML modules to ship, and our QML lives in `app/omnuv`. Nothing we
// could put in our own files would have told it otherwise, and the batch file
// belongs to upstream.
//
// So: no QML module to deploy, nothing for a scanner to miss, and a stable API
// instead of a labs one whose contract is explicitly provisional. It needs
// QApplication rather than QGuiApplication — which is the same change the tray
// already required, so the change budget grows by nothing.
class OmnuvTray : public QObject
{
    Q_OBJECT

public:
    // Returns nullptr where the platform has no tray at all, rather than
    // constructing something that silently does nothing.
    static OmnuvTray* createIfSupported(OmnuvSession* session, QObject* parent = nullptr);

private slots:
    void refresh();
    void openWindow();
    void showAbout();
    void activated(QSystemTrayIcon::ActivationReason reason);

private:
    explicit OmnuvTray(OmnuvSession* session, QObject* parent);

    // One line of state, and it is not clickable. A tray menu is not a
    // dashboard: putting machine rows here would be a second place where the
    // machine list is interpreted, and the rule is that the client displays
    // and Core decides.
    QString stateLine() const;

    OmnuvSession* m_session;
    QSystemTrayIcon* m_icon;
    QMenu* m_menu;
    QAction* m_state;
};
