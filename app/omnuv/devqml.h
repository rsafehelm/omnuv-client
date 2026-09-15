#pragma once

#include <QQmlEngine>

// QML from disk instead of from the compiled resources, for development.
//
// With `OMNUV_QML_DIR=<dir>` set, every `qrc:/omnuv/<file>` the engine loads
// — the pushes in main.qml, the composite types registered by URL, the
// `Qt.createComponent` calls — is served from `<dir>/<file>` instead. An edit
// to a .qml is then a restart away, not a compile away: on the rig that is
// the difference between thirty seconds and a cycle. Silent and inert when
// the variable is unset, which is every shipped configuration.
//
// What it cannot do: change C++ types, or anything qmlcachegen would have
// caught at build time — `scripts/client-check` runs qmllint for that.
namespace OmnuvDevQml {
void install(QQmlEngine* engine);
}
