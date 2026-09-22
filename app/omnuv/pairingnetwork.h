#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <atomic>
#include <memory>

using OmnuvPairingCancellation = std::shared_ptr<std::atomic_bool>;

// A worker-local network manager. The UI only writes the shared cancellation
// flag; replies are aborted on their owning thread, including PIN waits.
class OmnuvPairingNetwork : public QNetworkAccessManager
{
public:
    explicit OmnuvPairingNetwork(OmnuvPairingCancellation cancel = {})
        : m_cancel(std::move(cancel)) {}
    bool cancelled() const { return m_cancel && m_cancel->load(); }

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoing = nullptr) override
    {
        auto reply = QNetworkAccessManager::createRequest(op, request, outgoing);
        if (m_cancel) {
            auto timer = new QTimer(reply);
            timer->setInterval(25);
            connect(timer, &QTimer::timeout, reply, [cancel = m_cancel, reply]() {
                if (cancel->load() && !reply->isFinished()) reply->abort();
            });
            connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);
            timer->start();
        }
        return reply;
    }

private:
    OmnuvPairingCancellation m_cancel;
};
