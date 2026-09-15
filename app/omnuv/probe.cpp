#include "probe.h"

#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <chrono>

// Long enough for a deployment behind a slow link, short enough that the tray
// is never showing a reading older than one poll interval. A `/health` that
// has not answered in this is down as far as anybody standing here can tell,
// and saying so is the point of the row.
static constexpr std::chrono::milliseconds kHealthTimeout{8000};

OmnuvProbe::OmnuvProbe(QObject* parent)
    : QObject(parent)
{
    // **Asked once, and the answer is announced.** `loadDefaultBackend()`
    // resolves a plugin out of `plugins/networkinformation/`, and a plugin is
    // exactly the kind of thing that is present in the Qt installation and
    // absent from the deployed application — which is how every Windows build
    // of this fork once exited at startup. It cannot appear later, so asking
    // per poll would be a plugin scan a minute for a value that cannot change.
    //
    // `scripts/build-arch.bat` passes `--skip-plugin-types qmltooling,generic`
    // and nothing else, and `QT += network` is in `app.pro`, so windeployqt is
    // expected to bring it. Expected is not measured: the log line below is
    // what the rig reads, and a `backend=absent` is a deployment finding
    // rather than a silence.
    m_haveReachability = QNetworkInformation::loadDefaultBackend();

    QNetworkInformation* info = QNetworkInformation::instance();
    if (m_haveReachability && info != nullptr) {
        qInfo("probe=internet backend=%s", qUtf8Printable(info->backendName()));
        connect(info, &QNetworkInformation::reachabilityChanged,
                this, &OmnuvProbe::changed);
    }
    else {
        // Named, not bare. The next cause — a platform with no backend at all,
        // a plugin we learn to deploy — arrives as a different reason rather
        // than changing what this one means.
        m_haveReachability = false;
        qInfo("probe=internet backend=absent");
    }
}

omnuv::Reading OmnuvProbe::internet() const
{
    QNetworkInformation* info = QNetworkInformation::instance();
    if (!m_haveReachability || info == nullptr) {
        return omnuv::Reading::Unknown;
    }

    // Qt's five, mapped to the three a reading has. `Local` and `Site` are
    // failures rather than passes: both mean the operating system believes
    // this device can reach a network and not the internet, which is the
    // attribution this row exists to make. `Unknown` is Qt's own word for a
    // backend that will not say, and it stays unknown.
    switch (info->reachability()) {
    case QNetworkInformation::Reachability::Online:
        return omnuv::Reading::Pass;
    case QNetworkInformation::Reachability::Disconnected:
    case QNetworkInformation::Reachability::Local:
    case QNetworkInformation::Reachability::Site:
        return omnuv::Reading::Fail;
    case QNetworkInformation::Reachability::Unknown:
        break;
    }
    return omnuv::Reading::Unknown;
}

void OmnuvProbe::setCoreUrl(const QString& url)
{
    if (m_coreUrl == url) {
        return;
    }
    m_coreUrl = url;

    // The old reading was about a different deployment. Keeping it would be a
    // claim about somewhere nobody asked about.
    m_core = omnuv::Reading::Unknown;
    m_takenAt = QDateTime();
    emit changed();
}

void OmnuvProbe::setCore(omnuv::Reading reading)
{
    // The stamp moves even when the reading does not, because "we looked again
    // and it is still true" is a different fact from "we have not looked since
    // then", and the menu shows the difference.
    m_takenAt = QDateTime::currentDateTime();
    m_core = reading;
    emit changed();
}

void OmnuvProbe::check()
{
    if (m_coreUrl.isEmpty()) {
        // Nowhere to ask. Not a failure of Omnuv's — nobody has said which
        // Omnuv — so this stays unknown and takes no stamp.
        if (m_core != omnuv::Reading::Unknown) {
            m_core = omnuv::Reading::Unknown;
            m_takenAt = QDateTime();
            emit changed();
        }
        return;
    }

    // Unauthenticated, and deliberately built here rather than through
    // `OmnuvSession::request()`, which sets a bearer token on demand. This
    // request has no branch that could ever carry one.
    QNetworkRequest req{QUrl(m_coreUrl + QStringLiteral("/health"))};
    req.setTransferTimeout(kHealthTimeout);

    QNetworkReply* reply = m_net.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // A timeout is a failure here and not an unknown, and that is a
        // deliberate line. "I asked and nothing came back" is a reading —
        // Omnuv did not answer *this device* — which is exactly what the row
        // claims. Unknown is reserved for the case where no question was
        // asked at all, which is the branch above.
        setCore(reply->error() == QNetworkReply::NoError && status == 200
                    ? omnuv::Reading::Pass
                    : omnuv::Reading::Fail);
    });
}
