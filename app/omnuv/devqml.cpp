#include "devqml.h"

#include <QDir>
#include <QFile>
#include <QQmlAbstractUrlInterceptor>
#include <QtGlobal>

namespace {

class FromDisk final : public QQmlAbstractUrlInterceptor
{
public:
    explicit FromDisk(QString dir) : m_dir(std::move(dir)) {}

    QUrl intercept(const QUrl& url, DataType type) override
    {
        // Only files, only ours. Imports, qmldirs and upstream's own
        // `qrc:/gui/` are left exactly as built.
        if (type != QmlFile && type != JavaScriptFile) {
            return url;
        }
        if (url.scheme() != QLatin1String("qrc") || !url.path().startsWith(QLatin1String("/omnuv/"))) {
            return url;
        }
        const QString onDisk = m_dir + url.path().mid(int(sizeof("/omnuv/") - 1));
        return QFile::exists(onDisk) ? QUrl::fromLocalFile(onDisk) : url;
    }

private:
    QString m_dir;
};

} // namespace

void OmnuvDevQml::install(QQmlEngine* engine)
{
    const QString dir = QDir::fromNativeSeparators(qEnvironmentVariable("OMNUV_QML_DIR"));
    if (dir.isEmpty()) {
        return;
    }
    if (!QDir(dir).exists()) {
        qWarning("Omnuv: OMNUV_QML_DIR=%s does not exist; QML stays compiled in", qPrintable(dir));
        return;
    }
    // The interceptor outlives the engine on purpose: one engine per process,
    // and the process ends with it.
    engine->addUrlInterceptor(new FromDisk(dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/')));
    qInfo("Omnuv: QML from %s", qPrintable(dir));
}
