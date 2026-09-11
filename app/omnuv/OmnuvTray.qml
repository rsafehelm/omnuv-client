// The tray, which is the whole of being a widget.
//
// A widget is a program that was already running when you needed it. Everything
// else in this application already works; what was missing is that it had to be
// opened before it knew anything. This file, plus one line in main.cpp, is that
// difference.
//
// **A tray menu is not a dashboard.** Four entries and one line of state. The
// dashboard is a window, reached by clicking Open — putting machine rows in a
// menu would be a second place where the machine list is interpreted, and
// `packaging_and_friction.md` fixes the rule that the client displays and Core
// decides. One interpretation, in one view.

import QtQuick 2.9
import Qt.labs.platform 1.1 as Platform

import Omnuv 1.0

// Qt.labs.platform's SystemTrayIcon is the native one where the platform has
// one — freedesktop StatusNotifier on Ubuntu, NSStatusItem on macOS — and falls
// back to Qt Widgets elsewhere. That fallback is why `QT += widgets` and
// QApplication are required; see the change budget entry for main.cpp.
// The root is an Item purely so that the tray icon and the About dialog can
// both live here: `SystemTrayIcon` has no default property, so nothing may be
// declared inside it. Found by qmllint once it was given the module's import
// path — a checker that cannot resolve its imports reports every property as
// missing, which is indistinguishable from reporting nothing.
Item {
    id: root

    // The window this belongs to. Set by whoever instantiates it, so this file
    // knows nothing about where it lives.
    property var window: null

    visible: false

    function show() {
        if (!root.window) {
            return
        }
        root.window.show()
        root.window.raise()
        root.window.requestActivate()
    }

    Platform.SystemTrayIcon {
        id: tray

        visible: true
        icon.source: "qrc:/res/moonlight.svg"

        // What a person sees on hover, and the only state the tray carries. It says
        // what is true, including when the answer is that we cannot tell — a tray
        // that reads "Connected" because nothing has contradicted it yet is worse
        // than one that admits it does not know.
        tooltip: {
            if (!Omnuv.signedIn) {
                return qsTr("Omnuv — not signed in")
            }
            if (!Omnuv.tunnel.available) {
                return qsTr("Omnuv — network client not installed")
            }
            if (Omnuv.tunnel.busy) {
                return qsTr("Omnuv — %1").arg(Omnuv.tunnel.state)
            }
            return Omnuv.tunnel.connected
                ? qsTr("Omnuv — connected as %1").arg(Omnuv.tunnel.address)
                : qsTr("Omnuv — network not connected")
        }

        // Clicking the icon opens the window, which is what every tray application
        // on every platform does. Double-click is the same thing: a person who
        // double-clicks means the same as one who clicked.
        onActivated: function(reason) {
            if (reason === Platform.SystemTrayIcon.Trigger ||
                reason === Platform.SystemTrayIcon.DoubleClick) {
                root.show()
            }
    }

        menu: Platform.Menu {
            Platform.MenuItem {
                text: qsTr("Open Omnuv")
                onTriggered: root.show()
            }

            Platform.MenuSeparator {}

            // One line of state, and it is not clickable. It exists so that the
            // answer to "is my network up" costs a right-click rather than opening
            // a window.
            Platform.MenuItem {
                enabled: false
                text: {
                    if (!Omnuv.signedIn) {
                        return qsTr("Not signed in")
                    }
                    if (!Omnuv.tunnel.available) {
                        return qsTr("Network client not installed")
                    }
                    if (Omnuv.tunnel.connected) {
                        return qsTr("Network: %1").arg(Omnuv.tunnel.address)
                    }
                    return qsTr("Network: %1").arg(Omnuv.tunnel.state)
                }
            }

            Platform.MenuSeparator {}

            Platform.MenuItem {
                text: qsTr("About Omnuv Connect")
                // **Reachable in one click, and it names upstream.** This program
                // auto-starts, so a person who never deliberately launched it is
                // entitled to find out what it is and where its source lives. That
                // is the GPL's point and also simply the decent thing.
                //
                // The dialog lives in this file rather than in main.qml so the
                // upstream file keeps the smallest possible diff.
                onTriggered: about.open()
            }

            Platform.MenuItem {
                text: qsTr("Quit Omnuv")
                // The only thing that actually quits. Closing the window hides it;
                // this ends the program. A widget that cannot be quit from its own
                // tray is a widget people uninstall.
                onTriggered: Qt.quit()
            }
        }
    }

    Platform.MessageDialog {
        id: about
        title: qsTr("About Omnuv Connect")
        text: qsTr("Omnuv Connect")
        informativeText: qsTr(
            "Omnuv Connect is a fork of Moonlight, the open-source game " +
            "streaming client, with sign-in to Omnuv and the marketplace " +
            "network added.\n\n" +
            "Licensed GPL-3.0. Upstream: github.com/moonlight-stream/moonlight-qt\n" +
            "This fork, and what was changed: github.com/rsafehelm/omnuv-client")
        buttons: Platform.MessageDialog.Ok
    }
}
