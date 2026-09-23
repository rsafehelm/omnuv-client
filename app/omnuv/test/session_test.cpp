#include "../omnuvsession.h"
#include "../pairing.h"
#include "../../backend/nvpairingmanager.h"
#include "../../backend/nvcomputer.h"
#include <QElapsedTimer>
#include <QHostAddress>
#include <cmath>
#include <QNetworkReply>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>
#include <QFile>
#include <QDir>
#include <QQuickView>
#include <QQuickItem>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlComponent>

QAtomicInt g_AsyncLoggingEnabled;

// Requests can finish in any order. Tests exercise the real QNAM callbacks.
class HeldServer : public QTcpServer {
public:
    struct Call { QByteArray path, body; QPointer<QTcpSocket> socket; };
    QList<Call> calls;
    HeldServer() {
        listen(QHostAddress::LocalHost);
        connect(this,&QTcpServer::newConnection,this,[this]() {
            while (hasPendingConnections()) {
                auto socket=nextPendingConnection();
                auto buffer=std::make_shared<QByteArray>();
                auto captured=std::make_shared<bool>(false);
                connect(socket,&QTcpSocket::readyRead,this,[this,socket,buffer,captured]() {
                    *buffer += socket->readAll();
                    if (*captured) return;
                    int split=buffer->indexOf("\r\n\r\n");
                    if (split<0) return;
                    int length=0;
                    for (auto line:buffer->left(split).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length=line.mid(15).trimmed().toInt();
                    if (buffer->size()<split+4+length) return;
                    *captured=true;
                    calls.append({buffer->split(' ').at(1),buffer->mid(split+4,length),socket});
                });
            }
        });
    }
    QByteArray url() const { return "http://127.0.0.1:"+QByteArray::number(serverPort()); }
    int find(const QByteArray& path, int after=0) const {
        for (int i=after;i<calls.size();++i) if (calls[i].path==path) return i;
        return -1;
    }
    int count(const QByteArray& path) const {
        int n=0; for (const auto& call:calls) if(call.path==path) ++n; return n;
    }
    void answer(int i,int status,const QByteArray& body) {
        auto socket=calls[i].socket;
        if (!socket || socket->state()!=QAbstractSocket::ConnectedState) return;
        socket->write("HTTP/1.1 "+QByteArray::number(status)+" Result\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
        socket->disconnectFromHost();
    }
};

// Intercepts HTTPS in memory: no request can leave the test process.
class ControlledReply : public QNetworkReply {
    QByteArray bytes; qint64 offset=0;
public:
    bool aborted=false;
    ControlledReply(const QNetworkRequest& request,QNetworkAccessManager::Operation op,QObject* parent):QNetworkReply(parent) {
        setRequest(request); setUrl(request.url()); setOperation(op); open(QIODevice::ReadOnly);
    }
    void abort() override { aborted=true; setError(OperationCanceledError,"cancelled"); setFinished(true); emit finished(); }
    void complete(int status,const QByteArray& body) { bytes=body; setAttribute(QNetworkRequest::HttpStatusCodeAttribute,status); setFinished(true); emit readyRead(); emit finished(); }
    qint64 bytesAvailable() const override { return bytes.size()-offset+QNetworkReply::bytesAvailable(); }
    qint64 readData(char* data,qint64 max) override {
        auto n=qMin(max,qint64(bytes.size())-offset); if(n<=0) return -1;
        memcpy(data,bytes.constData()+offset,n); offset+=n; return n;
    }
};
class ControlledNetwork : public QNetworkAccessManager {
public:
    QList<QPointer<ControlledReply>> calls;
protected:
    QNetworkReply* createRequest(Operation op,const QNetworkRequest& request,QIODevice*) override {
        auto reply=new ControlledReply(request,op,this); calls.append(reply); return reply;
    }
};

// Public IPC protocol exercised without reaching the machine's installed daemon.
class FixtureTunnel {
public:
    QJsonObject membership;
    bool hasIdentity = false, verified = false;
    int state = 0;
    QString revision = "empty";
    QStringList commands;
    QString answer(const QString& line) {
        commands << line.section(' ', 0, 0); // never retain setup key payloads
        // Qualified by deployment since 22 September (`membership-v1 <core>`); the
        // bare form is what an older client asks. Answering only the bare form
        // made every enrollment test here fail with "update the network service".
        if (line == "membership-v1" || line.startsWith("membership-v1 ")) return QString::fromUtf8(QJsonDocument(QJsonObject{
            {"version",1},{"state",state},{"error",""},{"address","10.210.0.10"},{"has_identity",hasIdentity},{"verified",verified},{"pending",false},
            {"revision",revision},{"membership",membership.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(membership)}}).toJson(QJsonDocument::Compact));
        if (line == "state") return QString("state %1 10.210.0.10 fixture.internal").arg(state);
        if (line.startsWith("enrol-v1 ")) {
            const auto body = QJsonDocument::fromJson(QByteArray::fromBase64(line.section(' ',1).toLatin1())).object();
            if (body.value("expected_revision").toString() != revision) return "err membership changed";
            membership = body.value("membership").toObject(); hasIdentity = verified = true;
            revision = "joined"; state = 2; return "ok";
        }
        if (line.startsWith("resume-v1 ")) {
            const auto wanted = QJsonDocument::fromJson(QByteArray::fromBase64(line.section(' ',1).toLatin1())).object();
            if (!verified || wanted != membership) return "err membership mismatch";
            state = 2; return "ok";
        }
        if (line.startsWith("stop-v1 ")) { state = 0; return "ok"; }
        return "err unsupported";
    }
};

class OmnuvSessionTest : public QObject {
    Q_OBJECT
    static QJsonObject machine(const char* id,const char* host) {
        return {{"id",id},{"name",id},{"private_name",host},{"status","Running"},{"stream_app","Desktop"}};
    }
    void prepare(OmnuvSession& s) {
        s.m_refreshTimer.stop(); s.m_windowVisible=false;
        s.m_identityKnown=true; s.m_projectId="a"; s.m_accountId="account-a";
        s.m_tunnel->m_request=[](const QString& line) { FixtureTunnel daemon; return daemon.answer(line); };
        s.m_projectIds={"a","b"}; s.m_projectNames={"A","B"};
        s.updateNetworkScope();
    }
private slots:
    void materialLabelsAndWindowUseTheSamePaletteInBothThemes() {
        QFile source("/src/app/gui/main.qml"); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto text=QString::fromUtf8(source.readAll());
        QString bindings;
        for (const auto& line:text.split('\n')) {
            if (line.trimmed().startsWith("Material.theme:") || line.trimmed().startsWith("Material.background:")) bindings+=line+"\n";
        }
        QVERIFY(bindings.contains("Material.theme:")); QVERIFY(bindings.contains("Material.background:"));
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); QQmlEngine engine; QQmlComponent component(&engine);
        component.setData(("import QtQuick\nimport QtQuick.Controls.Material\nApplicationWindow { width: 700; height: 220; visible: true; property bool dark: false; palette.window: dark ? '#303030' : '#fafafa'; property alias labelColor: label.color;\n"
            +bindings+"Label { id: label; anchors.centerIn: parent; text: 'Project research — GPU workstation'; font.pixelSize: 24 }\n}").toUtf8(),QUrl("qrc:/omnuv/MaterialPaletteTest.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(),2000); QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        auto object=component.create(); QVERIFY2(object,qPrintable(component.errorString()));
        std::unique_ptr<QObject> owned(object); auto window=qobject_cast<QQuickWindow*>(object); QVERIFY(window);
        const auto luminance=[](QColor color) {
            const auto channel=[](double value) { return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4); };
            return .2126*channel(color.redF())+.7152*channel(color.greenF())+.0722*channel(color.blueF());
        };
        for (const bool dark : {false,true,false}) {
            object->setProperty("dark",dark); QTest::qWait(100);
            const auto foreground=object->property("labelColor").value<QColor>();
            const auto background=window->color();
            const double fg=luminance(foreground), bg=luminance(background);
            QVERIFY2((qMax(fg,bg)+.05)/(qMin(fg,bg)+.05)>=4.5,"Material body text must contrast with the actual window background");
            QCOMPARE(background,QColor(dark ? "#303030" : "#fafafa"));
            QVERIFY(dark ? foreground.lightnessF()>.5 : foreground.lightnessF()<.5);
            auto shot=window->grabWindow(); QVERIFY(!shot.isNull());
            QVERIFY(shot.save(qEnvironmentVariable("OMNUV_TEST_ARTIFACT_DIR",QDir::tempPath())+(dark ? "/material-dark.png" : "/material-light.png")));
        }
    }
    void persistenceNoticeRendersAndTracksTheSessionWarning() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.m_storeToken=[](const QString&) { return false; }; s.saveToken("fixture-only");
        QFile source("/src/app/omnuv/OmnuvView.qml"); QVERIFY(source.open(QIODevice::ReadOnly));
        auto text=QString::fromUtf8(source.readAll());
        const auto begin=text.indexOf("    component Notice: Rectangle {");
        const auto end=text.indexOf("    // Nothing to show yet",begin);
        QVERIFY(begin>=0 && end>begin);
        const auto bindingBegin=text.lastIndexOf("        Notice {",text.indexOf("objectName: \"credentialWarning\""));
        const auto bindingEnd=text.indexOf("        }",bindingBegin)+9;
        QVERIFY(bindingBegin>=0 && bindingEnd>bindingBegin);
        auto binding=text.mid(bindingBegin,bindingEnd-bindingBegin).replace("Omnuv.","sample.");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        QQuickView view;
        view.engine()->rootContext()->setContextProperty("sample",&s);
        QQmlComponent component(view.engine());
        component.setData(("import QtQuick\nimport QtQuick.Controls\nimport QtQuick.Layouts\nimport Omnuv 1.0\nRectangle { width: 900; height: 160; color: \"white\"\n"
            +text.mid(begin,end-begin)+"\nColumnLayout { anchors.fill: parent; anchors.margins: 16;\n"+binding+"\n}\n}").toUtf8(),QUrl("qrc:/omnuv/NoticeTest.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(),2000);
        QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        auto object=component.create(); QVERIFY2(object,qPrintable(component.errorString()));
        view.setContent(QUrl("qrc:/omnuv/NoticeTest.qml"),&component,object); view.show();
        auto notice=object->findChild<QQuickItem*>("credentialWarning"); QVERIFY(notice);
        QTRY_VERIFY(notice->isVisible()); QCOMPARE(notice->property("text").toString(),s.credentialWarning());
        QTest::qWait(150); auto screenshot=view.grabWindow(); QVERIFY(!screenshot.isNull());
        const auto output=qEnvironmentVariable("OMNUV_TEST_ARTIFACT_DIR",QDir::tempPath())+"/credential-warning.png";
        QVERIFY(screenshot.save(output));
        s.setStatus(QString()); QVERIFY(notice->isVisible());
        s.m_storeToken=[](const QString&) { return true; }; s.saveToken("fixture-only");
        QTRY_VERIFY(!notice->isVisible());
    }
    static QByteArray identity(const QJsonArray& projects = QJsonArray{QJsonObject{{"id","a"},{"name","A"}}}) {
        return QJsonDocument(QJsonObject{{"user_id","account-a"},{"email","buyer@example.test"},{"organization_name","Fixture"},
            {"organization_role","owner"},{"projects",projects}}).toJson();
    }
    void pairingCertificateWaitIsBoundedAgainstSilentLocalPeer() {
        HeldServer server;
        NvComputer computer{};
        computer.activeAddress=NvAddress("127.0.0.1", server.serverPort());
        computer.activeHttpsPort=server.serverPort();
        computer.isNvidiaServerSoftware=false;
        NvPairingManager pairing(&computer); QSslCertificate certificate;
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY_EXCEPTION_THROWN(pairing.pair("7.0.0.0", "1234", certificate, 150), QtNetworkReplyException);
        QVERIFY(elapsed.elapsed() < 3000);
        QVERIFY(!server.calls.isEmpty());
        QVERIFY(certificate.isNull());
        // Zero also cannot disable the deadline through the internal API.
        elapsed.restart();
        QVERIFY_EXCEPTION_THROWN(pairing.pair("7.0.0.0", "1234", certificate, 0), QtNetworkReplyException);
        QVERIFY(elapsed.elapsed() < 3000);
    }
    void pairingCancellationInterruptsTheNativeCertificateWait() {
        HeldServer server; NvComputer computer{};
        computer.activeAddress=NvAddress("127.0.0.1",server.serverPort());
        computer.activeHttpsPort=server.serverPort(); computer.isNvidiaServerSoftware=false;
        auto cancel=std::make_shared<std::atomic_bool>(false);
        NvPairingManager pairing(&computer,cancel); QSslCertificate certificate;
        QTimer::singleShot(50,&server,[cancel]() { cancel->store(true); });
        QElapsedTimer elapsed; elapsed.start();
        QVERIFY_EXCEPTION_THROWN(pairing.pair("7.0.0.0","1234",certificate),QtNetworkReplyException);
        QVERIFY(elapsed.elapsed()<2000); QVERIFY(!server.calls.isEmpty()); QVERIFY(certificate.isNull());
        // Cancellation before the worker starts cannot issue another request.
        const auto requests=server.calls.size();
        QCOMPARE(pairing.pair("7.0.0.0","1234",certificate),NvPairingManager::FAILED);
        QCOMPARE(server.calls.size(),requests);
    }
    void enrollmentCauseSurvivesPollingAndClearsOnSuccessfulRetry() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon;
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->check(); s.m_tunnel->join();
        QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        auto networks=server.find("/v1/networks?project=a");
        server.answer(networks,409,R"({"error":"network is still being prepared"})");
        QTRY_VERIFY(!s.m_tunnel->busy());
        const auto problem=s.m_tunnel->operationError();
        QVERIFY(problem.contains("409")); QVERIFY(problem.contains("still being prepared"));
        s.m_tunnel->check(); s.setStatus(QString());
        QCOMPARE(s.m_tunnel->operationError(),problem);
        QVERIFY(s.m_tunnel->state()!=problem); // observations remain independent
        const auto offset=server.calls.size(); s.m_tunnel->join();
        QVERIFY(s.m_tunnel->operationError().isEmpty());
        QTRY_VERIFY(server.find("/v1/networks?project=a",offset)>=0);
        networks=server.find("/v1/networks?project=a",offset);
        server.answer(networks,200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
        server.answer(server.find("/v1/networks/network-a/devices"),200,
            QJsonDocument(QJsonObject{{"id",QJsonDocument::fromJson(server.calls[server.find("/v1/networks/network-a/devices")].body).object().value("attempt_id")},{"setup_key","fixture-only"}, {"command","OmnuvClient enrol fixture-only --management-url https://overlay.test.invalid"}}).toJson());
        QTRY_VERIFY(s.m_tunnel->connected()); QVERIFY(!s.m_tunnel->busy());
        QVERIFY(s.m_tunnel->operationError().isEmpty());
    }
    void enrollmentCreationRefusalRetainsItsCauseAtTheIdleTransition() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        QString idleReason;
        connect(s.m_tunnel,&OmnuvTunnel::changed,this,[&]() {
            if (!s.m_tunnel->busy() && !s.m_tunnel->operationError().isEmpty()) idleReason=s.m_tunnel->state();
        });
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
        server.answer(server.find("/v1/networks/network-a/devices"),403,R"({"error":"device enrollment is not permitted"})");
        QTRY_VERIFY(!s.m_tunnel->busy());
        QVERIFY(idleReason.contains("not permitted")); QVERIFY(idleReason.contains("403"));
        QVERIFY(s.signedIn()); s.m_tunnel->check();
        QVERIFY(s.m_tunnel->operationError().contains("not permitted"));
        s.selectProject("b"); QVERIFY(s.m_tunnel->operationError().isEmpty());
    }
    void incompatibleEnrollmentNetworkListDoesNotCreateADevice() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.fetchDeviceKey(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"({"id":"not-an-array"})");
        QTRY_VERIFY(s.m_tunnel->operationError().contains("cannot read"));
        s.m_tunnel->check(); QVERIFY(s.m_tunnel->operationError().contains("cannot read"));
        for (const auto& call:server.calls) QVERIFY(!call.path.endsWith("/devices"));
    }
    void enrollmentAuthenticationFailureOffersSignInAgain() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.fetchDeviceKey(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),401,R"({"error":"device token expired"})");
        QTRY_VERIFY(!s.signedIn()); QVERIFY(s.status().contains("Sign in again"));
        QVERIFY(s.status().contains("expired")); QVERIFY(s.m_tunnel->operationError().contains("401"));
    }
    void anUnverifiedProductionIdentityIsNeverResumedOrMovedWithoutConfirmation() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.state=2; daemon.hasIdentity=true; daemon.revision="production-existing";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->check(); QVERIFY(!s.m_tunnel->connected());
        s.m_tunnel->resume(); QVERIFY(!daemon.commands.contains("resume-v1"));
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(!s.networkMovePrompt().isEmpty());
        QVERIFY(s.networkMovePrompt().contains("unverified")); QVERIFY(s.networkMovePrompt().contains("revoke"));
        QCOMPARE(server.count("/v1/networks/network-a/devices"),0);
        s.cancelNetworkMove(); QVERIFY(s.networkMovePrompt().isEmpty());
        QVERIFY(!daemon.commands.contains("enrol-v1")); QCOMPARE(daemon.revision,QString("production-existing"));
        s.selectProject("b"); QVERIFY(!s.m_tunnel->connected()); QVERIFY(!daemon.commands.contains("resume-v1"));
    }
    void explicitMoveUsesTheConfirmedRevisionAndReconfirmsChanges() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.state=2; daemon.hasIdentity=true; daemon.revision="first-identity";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(!s.networkMovePrompt().isEmpty());
        const auto second=server.calls.size(); daemon.revision="changed-before-confirmation";
        s.confirmNetworkMove(); QTRY_VERIFY(server.find("/v1/networks?project=a",second)>=0);
        server.answer(server.find("/v1/networks?project=a",second),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(!s.networkMovePrompt().isEmpty()); QCOMPARE(server.count("/v1/networks/network-a/devices"),0);
        const auto third=server.calls.size(); s.confirmNetworkMove();
        QTRY_VERIFY(server.find("/v1/networks?project=a",third)>=0);
        server.answer(server.find("/v1/networks?project=a",third),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
        const auto post=server.find("/v1/networks/network-a/devices");
        const auto attempt=QJsonDocument::fromJson(server.calls[post].body).object().value("attempt_id").toString();
        server.answer(post,200,QJsonDocument(QJsonObject{{"id",attempt},{"setup_key","one-time-only"},
            {"command","OmnuvClient enrol one-time-only --management-url https://overlay.test.invalid"}}).toJson());
        QTRY_VERIFY(s.m_tunnel->connected()); QCOMPARE(daemon.membership.value("device_id").toString(),attempt);
        QVERIFY(s.enrollmentRecovery().isEmpty());
    }
    void anExistingRunningTunnelCannotCompleteAPendingJoinBeforeItsScopeIsChecked() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.state=2; daemon.hasIdentity=daemon.verified=true;
        daemon.membership=s.networkScope(); daemon.membership["network_id"]="network-a"; daemon.membership["device_id"]="device-a";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->join(); s.m_tunnel->check();
        QVERIFY(s.m_tunnel->busy()); QVERIFY(!s.m_tunnel->connected());
        QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(s.m_tunnel->connected()); QVERIFY(!s.m_tunnel->busy());
    }
    void onlyTheSelectedAccountDeploymentAndProjectCanReportConnected() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.state=2; daemon.hasIdentity=daemon.verified=true;
        daemon.membership=s.networkScope(); daemon.membership["network_id"]="network-a"; daemon.membership["device_id"]="device-a";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->check(); QVERIFY(s.m_tunnel->connected());
        s.selectProject("b"); QVERIFY(!s.m_tunnel->connected());
        s.selectProject("a"); QVERIFY(s.m_tunnel->connected());
        s.m_accountId="account-b"; s.updateNetworkScope(); QVERIFY(!s.m_tunnel->connected());
        s.m_accountId="account-a"; s.m_coreUrl="https://other.example.test"; s.updateNetworkScope(); QVERIFY(!s.m_tunnel->connected());
        QVERIFY(!daemon.commands.contains("resume-v1")); QVERIFY(!daemon.commands.contains("enrol-v1"));
        QCOMPARE(daemon.membership.value("account_id").toString(),QString("account-a"));
    }
    void aMatchingMembershipResumesWithoutCreatingAnotherDevice() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.hasIdentity=daemon.verified=true;
        daemon.membership=s.networkScope(); daemon.membership["network_id"]="network-a"; daemon.membership["device_id"]="device-a";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(s.m_tunnel->connected()); QVERIFY(daemon.commands.contains("resume-v1"));
        QCOMPARE(server.count("/v1/networks/network-a/devices"),0);
    }
    void lostEnrollmentResponseKeepsTheSameAttemptAcrossClientRestart() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); const auto path=dir.path()+"/settings.ini";
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); QString first;
        {
            OmnuvSession s; prepare(s); s.m_enrollmentJournal=OmnuvEnrollmentJournal(false,path);
            s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
            server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
            QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
            auto post=server.find("/v1/networks/network-a/devices");
            first=QJsonDocument::fromJson(server.calls[post].body).object().value("attempt_id").toString();
            QVERIFY(!QUuid(first).isNull());
            // The server may have committed; the local response is lost.
            server.answer(post,503,R"({"error":"upstream response lost"})");
            QTRY_VERIFY(!s.m_tunnel->busy()); QVERIFY(s.enrollmentRecovery().contains(first));
        }
        const auto offset=server.calls.size();
        OmnuvSession s; prepare(s); s.m_enrollmentJournal=OmnuvEnrollmentJournal(false,path);
        QVERIFY(s.enrollmentRecovery().contains(first)); s.m_tunnel->join();
        QTRY_VERIFY(server.find("/v1/networks?project=a",offset)>=0);
        server.answer(server.find("/v1/networks?project=a",offset),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices",offset)>=0);
        auto post=server.find("/v1/networks/network-a/devices",offset);
        QCOMPARE(QJsonDocument::fromJson(server.calls[post].body).object().value("attempt_id").toString(),first);
        server.answer(post,409,R"({"error":"a key was already issued; revoke this attempt"})");
        QTRY_VERIFY(s.m_tunnel->operationError().contains("already issued"));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes=file.readAll();
        QVERIFY(bytes.contains(first.toUtf8())); QVERIFY(!bytes.contains("setup_key")); QVERIFY(!bytes.contains("fixture-token"));
    }
    void cancellationFencesALateCreateAndOnlyThenAllowsANewAttempt() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
        auto post=server.find("/v1/networks/network-a/devices");
        const auto attempt=QJsonDocument::fromJson(server.calls[post].body).object().value("attempt_id").toString();
        s.revokePendingEnrollment(); const auto cancel=QByteArray("/v1/networks/network-a/devices/")+attempt.toUtf8();
        QTRY_VERIFY(server.find(cancel)>=0); server.answer(server.find(cancel),204,"");
        QTRY_VERIFY(s.enrollmentRecovery().isEmpty());
        server.answer(post,200,QJsonDocument(QJsonObject{{"id",attempt},{"setup_key","never-persist"},{"command","OmnuvClient enrol never-persist --management-url https://overlay.test.invalid"}}).toJson());
        QTest::qWait(50); QVERIFY(!daemon.commands.contains("enrol-v1"));
        const auto offset=server.calls.size(); s.m_tunnel->join();
        QTRY_VERIFY(server.find("/v1/networks?project=a",offset)>=0);
        server.answer(server.find("/v1/networks?project=a",offset),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices",offset)>=0);
        const auto next=QJsonDocument::fromJson(server.calls[server.find("/v1/networks/network-a/devices",offset)].body).object().value("attempt_id").toString();
        QVERIFY(next!=attempt); QVERIFY(!QUuid(next).isNull());
    }
    void missingDaemonEndsTheWaitAndPreservesTheEnrollmentForRecovery() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        auto scope=s.networkScope(); scope["network_id"]="network-a"; QJsonObject record;
        QVERIFY(s.m_enrollmentJournal.reserve(scope,&record));
        const auto attempt=record.value("membership").toObject().value("device_id").toString();
        const auto before=s.m_enrollmentRequest;
        s.m_tunnel->beginOperation("Joining");
        s.m_tunnel->m_request=[](const QString&) { return QString(); };
        QVERIFY(QMetaObject::invokeMethod(&s.m_tunnel->m_operationDeadline,"timeout"));
        QVERIFY(!s.m_tunnel->busy()); QVERIFY(s.m_tunnel->operationError().contains("not confirmed"));
        QVERIFY(s.enrollmentRecovery().contains(attempt)); QVERIFY(s.m_enrollmentRequest>before);
        s.m_tunnel->check(); QVERIFY(s.m_tunnel->operationError().contains("not confirmed"));
    }
    void unsupportedCancellationKeepsRecoveryDetails() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        auto scope=s.networkScope(); scope["network_id"]="network-a"; QJsonObject record;
        QVERIFY(s.m_enrollmentJournal.reserve(scope,&record));
        const auto id=record.value("membership").toObject().value("device_id").toString();
        s.revokePendingEnrollment(); const auto path=QByteArray("/v1/networks/network-a/devices/")+id.toUtf8();
        QTRY_VERIFY(server.find(path)>=0); server.answer(server.find(path),404,R"({"error":"unknown endpoint"})");
        QTRY_VERIFY(s.m_tunnel->operationError().contains("not confirmed"));
        QVERIFY(s.enrollmentRecovery().contains(id));
    }
    void oldDaemonIsRefusedBeforeAnyEnrollmentRequest() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        QStringList commands;
        s.m_tunnel->m_request=[&](const QString& line) { commands<<line; return line=="state" ? QString("state 2 10.1.2.3 old") : QString("err unknown request"); };
        s.m_tunnel->join(); QVERIFY(s.m_tunnel->operationError().contains("Update"));
        QVERIFY(!s.m_tunnel->connected()); QVERIFY(!commands.contains("resume"));
        for (const auto& call:server.calls) QVERIFY(!call.path.startsWith("/v1/networks"));
    }
    void oldCoreEnrollmentIdIsRetainedForCleanupWithoutChangingTheTunnel() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->join(); QTRY_VERIFY(server.find("/v1/networks?project=a")>=0);
        server.answer(server.find("/v1/networks?project=a"),200,R"([{"id":"network-a"}])");
        QTRY_VERIFY(server.find("/v1/networks/network-a/devices")>=0);
        server.answer(server.find("/v1/networks/network-a/devices"),200,QJsonDocument(QJsonObject{{"id","old-generated-id"},{"setup_key","never-persist"},{"command","unused"}}).toJson());
        QTRY_VERIFY(s.m_tunnel->operationError().contains("does not support"));
        QVERIFY(s.enrollmentRecovery().contains("old-generated-id")); QVERIFY(!daemon.commands.contains("enrol-v1"));
    }
    void contextSwitchDuringCleanupNeverDispatchesToTheNewDeployment() {
        HeldServer original, replacement; qputenv("OMNUV_FIXTURE_URL",original.url()); OmnuvSession s; prepare(s);
        auto scope=s.networkScope(); scope["network_id"]="network-a"; QJsonObject record;
        QVERIFY(s.m_enrollmentJournal.reserve(scope,&record)); record["unexpected_device_id"]="old-generated-id";
        const auto member=record.value("membership").toObject();
        QVERIFY(s.m_enrollmentJournal.put(OmnuvEnrollmentJournal::key(member),record));
        s.revokePendingEnrollment(); const auto path=QByteArray("/v1/networks/network-a/devices/")+member.value("device_id").toString().toUtf8();
        QTRY_VERIFY(original.find(path)>=0);
        s.setCoreUrl(QString::fromUtf8(replacement.url()));
        original.answer(original.find(path),204,""); QTest::qWait(50);
        QCOMPARE(replacement.calls.size(),0);
        QCOMPARE(original.count("/v1/devices/old-generated-id"),0);
        QVERIFY(!s.m_enrollmentJournal.records().isEmpty());
    }
    void explicitMoveDialogRendersTheOriginalMembershipAndCancellation() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.m_networkMovePrompt="Move to Project B? Revoke device old-device in Project A at https://original.example.test first.";
        QFile source("/src/app/omnuv/OmnuvView.qml"); QVERIFY(source.open(QIODevice::ReadOnly));
        const auto text=QString::fromUtf8(source.readAll());
        const auto begin=text.lastIndexOf("    Dialog {",text.indexOf("id: networkMove"));
        const auto end=text.indexOf("    Dialog {",begin+1); QVERIFY(begin>=0 && end>begin);
        auto dialog=text.mid(begin,end-begin).replace("Omnuv.","sample.");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software); QQuickView view;
        view.engine()->rootContext()->setContextProperty("sample",&s); QQmlComponent component(view.engine());
        component.setData(("import QtQuick\nimport QtQuick.Controls\nRectangle { id: root; width: 900; height: 500; color: \"white\"\n"+dialog+"\n}").toUtf8(),QUrl("qrc:/omnuv/MoveTest.qml"));
        QTRY_VERIFY_WITH_TIMEOUT(!component.isLoading(),2000); QVERIFY2(component.isReady(),qPrintable(component.errorString()));
        auto object=component.create(); QVERIFY2(object,qPrintable(component.errorString()));
        view.setContent(QUrl("qrc:/omnuv/MoveTest.qml"),&component,object); view.show();
        auto popup=object->findChild<QObject*>("networkMove"); QVERIFY(popup); QVERIFY(QMetaObject::invokeMethod(popup,"open"));
        QTRY_VERIFY(popup->property("visible").toBool()); QTest::qWait(100);
        auto shot=view.grabWindow(); QVERIFY(!shot.isNull());
        QVERIFY(shot.save(qEnvironmentVariable("OMNUV_TEST_ARTIFACT_DIR",QDir::tempPath())+"/network-move-confirmation.png"));
        QVERIFY(QMetaObject::invokeMethod(popup,"reject")); QTRY_VERIFY(s.networkMovePrompt().isEmpty());
    }
    void enrollmentJournalIsDurableScopedAndRefusesBrokenStorage() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QJsonObject scope{{"core_url","https://core.example.test"},{"account_id","account-a"},{"project_id","project-a"},{"network_id","network-a"}};
        OmnuvEnrollmentJournal first(false,dir.path()+"/saved.ini"); QJsonObject a,b;
        QVERIFY(first.reserve(scope,&a)); OmnuvEnrollmentJournal reopened(false,dir.path()+"/saved.ini");
        QVERIFY(reopened.reserve(scope,&b)); QCOMPARE(a,b);
        auto other=scope; other["account_id"]="account-b"; QVERIFY(reopened.reserve(other,&b)); QVERIFY(a!=b);
        QFile blocker(dir.path()+"/not-a-directory"); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        OmnuvEnrollmentJournal broken(false,blocker.fileName()+"/settings.ini"); QVERIFY(!broken.reserve(scope,&b));
        QFile corrupt(dir.path()+"/corrupt.ini"); QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("[omnuv]\nenrollments=broken-json\n"); corrupt.close();
        OmnuvEnrollmentJournal damaged(false,corrupt.fileName()); QVERIFY(!damaged.reserve(scope,&b));
    }
    void enrollmentWithoutSelectedProjectDoesNotUseTheDefaultProject() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.m_projectId.clear(); s.fetchDeviceKey();
        QVERIFY(s.m_tunnel->operationError().contains("Choose a project"));
        for (const auto& call:server.calls) QVERIFY(!call.path.startsWith("/v1/networks"));
    }
    void failedCredentialRemovalIsVisibleAfterSignoutAndRevocation() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL", server.url()); OmnuvSession s;
        s.m_clearToken=[]() { return false; };
        s.signOut(); QVERIFY(!s.signedIn()); QVERIFY(!s.credentialWarning().isEmpty());
        const auto warning=s.credentialWarning(); s.setStatus(QString()); s.refresh(true);
        QCOMPARE(s.credentialWarning(), warning);
        s.m_token="fixture"; s.accessTakenBack();
        QVERIFY(!s.signedIn()); QVERIFY(s.credentialWarning().contains("revoked"));
        s.m_clearToken=[]() { return true; }; s.signOut();
        QVERIFY(s.credentialWarning().isEmpty());
    }
    void credentialWarningSurvivesSignInIdentityAndMachineRefresh() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); int saves=0;
        s.m_storeToken=[&saves](const QString&) { ++saves; return false; };
        s.m_deviceCode="fixture-code"; s.poll();
        QTRY_VERIFY(server.find("/v1/auth/device/token")>=0);
        int firstIdentity=server.find("/v1/me");
        server.answer(server.find("/v1/auth/device/token"),200,R"({"token":"fixture-new-token"})");
        QTRY_VERIFY(s.signedIn()); QCOMPARE(saves,1);
        QVERIFY(!s.credentialWarning().isEmpty()); const auto warning=s.credentialWarning();
        QTRY_VERIFY(server.find("/v1/me",firstIdentity+1)>=0);
        s.m_windowVisible=false;
        server.answer(server.find("/v1/me",firstIdentity+1),200,identity());
        QTRY_VERIFY(server.find("/v1/instances?project=a")>=0);
        server.answer(server.find("/v1/instances?project=a"),200,"[]");
        QTRY_VERIFY(s.m_identityKnown);
        QTest::qWait(50); QCOMPARE(s.credentialWarning(),warning);
        QCOMPARE(s.property("credentialWarning").toString(),warning);
        QVERIFY(s.status().isEmpty());
        s.m_storeToken=[](const QString&) { return true; }; s.saveToken("fixture-only");
        QVERIFY(s.credentialWarning().isEmpty());
        s.m_storeToken=[](const QString&) { return false; }; s.saveToken("fixture-only");
        s.signOut(); QVERIFY(s.credentialWarning().isEmpty());
    }
    void invalidIdentityPreservesLastGoodState_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("syntax")<<QByteArray("{");
        QTest::newRow("array")<<QByteArray("[]");
        QTest::newRow("missing")<<QByteArray("{}");
        auto object=QJsonDocument::fromJson(identity()).object();
        object["projects"]=QJsonObject{};
        QTest::newRow("projects-object")<<QJsonDocument(object).toJson();
        object["projects"]=QJsonArray{42};
        QTest::newRow("project-not-object")<<QJsonDocument(object).toJson();
        object["projects"]=QJsonArray{QJsonObject{{"id","a"}}};
        QTest::newRow("missing-name")<<QJsonDocument(object).toJson();
        auto p=QJsonObject{{"id","a"},{"name","A"}};
        object["projects"]=QJsonArray{p,p};
        QTest::newRow("duplicate-id")<<QJsonDocument(object).toJson();
        object=QJsonDocument::fromJson(identity()).object(); object["email"]=42;
        QTest::newRow("email-type")<<QJsonDocument(object).toJson();
    }
    void invalidIdentityPreservesLastGoodState() {
        QFETCH(QByteArray,body); HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.m_accountEmail="previous@example.test"; s.machines()->replace({machine("a","a.internal")});
        QTRY_VERIFY(server.find("/v1/me")>=0);
        server.answer(server.find("/v1/me"),200,body);
        QTRY_VERIFY(!s.readProblem().isEmpty());
        QCOMPARE(s.projectId(),QString("a")); QCOMPARE(s.accountEmail(),QString("previous@example.test"));
        QCOMPARE(s.machines()->idAt(0),QString("a")); QVERIFY(!s.noProject());
        auto problem=s.readProblem();
        s.refresh(); QTRY_VERIFY(server.find("/v1/instances?project=a")>=0);
        server.answer(server.find("/v1/instances?project=a"),200,"[]");
        QTRY_COMPARE(s.machines()->rowCount(),0); QCOMPARE(s.readProblem(),problem);
        int count=server.calls.size(); s.retryReads();
        QTRY_VERIFY(server.find("/v1/me",count)>=0);
        server.answer(server.find("/v1/me",count),200,identity({}));
        QTRY_VERIFY(s.noProject()); QVERIFY(s.readProblem().isEmpty());
    }
    void invalidInitialIdentityDoesNotMeanNoProjects() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        QTRY_VERIFY(server.find("/v1/me")>=0); server.answer(server.find("/v1/me"),200,"{}");
        QTRY_VERIFY(!s.readProblem().isEmpty()); QVERIFY(!s.m_identityKnown); QVERIFY(!s.noProject());
        QCOMPARE(server.find("/v1/instances"),-1);
    }
    void invalidMachineListPreservesRows_data() {
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("syntax")<<QByteArray("[");
        QTest::newRow("object")<<QByteArray("{}");
        QTest::newRow("null")<<QByteArray("null");
        QTest::newRow("scalar-entry")<<QByteArray("[42]");
        auto m=machine("a","a.internal"); m.remove("status");
        QTest::newRow("missing-status")<<QJsonDocument(QJsonArray{m}).toJson();
        m=machine("a","a.internal"); m["private_name"]=42;
        QTest::newRow("host-type")<<QJsonDocument(QJsonArray{m}).toJson();
        m=machine("a","a.internal");
        QTest::newRow("duplicate-id")<<QJsonDocument(QJsonArray{m,m}).toJson();
    }
    void invalidMachineListPreservesRows() {
        QFETCH(QByteArray,body); HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.machines()->replace({machine("a","a.internal")}); s.refresh();
        QTRY_VERIFY(server.find("/v1/instances?project=a")>=0);
        int first=server.find("/v1/instances?project=a"); server.answer(first,200,body);
        QTRY_VERIFY(!s.readProblem().isEmpty()); QCOMPARE(s.machines()->idAt(0),QString("a"));
        s.refresh(); QTRY_VERIFY(server.find("/v1/instances?project=a",first+1)>=0);
        server.answer(server.find("/v1/instances?project=a",first+1),200,"[]");
        QTRY_COMPARE(s.machines()->rowCount(),0); QVERIFY(s.readProblem().isEmpty());
    }
    void additiveAndPendingMachineFieldsRemainCompatible() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        auto m=machine("a",""); m["private_name"]=QJsonValue::Null; m["stream_app"]=QJsonValue::Null;
        m["status"]="FutureState"; m["future_field"]=QJsonObject{{"nested",true}};
        s.refresh(); QTRY_VERIFY(server.find("/v1/instances?project=a")>=0);
        server.answer(server.find("/v1/instances?project=a"),200,QJsonDocument(QJsonArray{m}).toJson());
        QTRY_COMPARE(s.machines()->idAt(0),QString("a")); QVERIFY(s.readProblem().isEmpty());
    }
    void fixturePreservesSavedAccountAndFirstRun_data() {
        QTest::addColumn<bool>("hasRun"); QTest::newRow("first-run")<<false; QTest::newRow("later-run")<<true;
    }
    void fixturePreservesSavedAccountAndFirstRun() {
        QFETCH(bool,hasRun); QSettings settings;
        settings.setValue("omnuv/hasRunBefore",hasRun); settings.setValue("omnuv/coreUrl","https://saved.invalid");
        settings.setValue("omnuv/projectId","saved-project"); settings.sync();
        const auto keys=settings.allKeys(); QVariantMap before;
        for(const auto& key:keys) before[key]=settings.value(key);
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        QCOMPARE(s.m_token,QString("fixture-token")); QCOMPARE(s.coreUrl(),QString::fromUtf8(server.url()));
        QVERIFY(!s.shouldStartHidden()); QVERIFY(!s.shouldStartHidden());
        QVERIFY(!s.autostart()->supported()); QVERIFY(!s.autostart()->enabled());
        s.autostart()->setEnabled(true); s.autostart()->setEnabled(false);
        s.saveToken("fixture-not-persisted"); s.signOut(); s.setCoreUrl("http://127.0.0.1:1");
        settings.sync(); QVariantMap after;
        for(const auto& key:settings.allKeys()) after[key]=settings.value(key);
        QCOMPARE(after,before);
    }
    void pendingPairingCanArriveLateAndCancellationStopsRetries() {
        ControlledNetwork net; OmnuvPairing pairing(&net);
        QSignalSpy delivered(&pairing,&OmnuvPairing::delivered);
        // An overlay address: since BUYER-12 the login is sent to nothing else,
        // and the name is resolved first, so the request is asynchronous.
        pairing.deliver("10.208.0.9","1234","test","10.0.0.1","test","fixture-only");
        QTRY_COMPARE(net.calls.size(),1);
        net.calls[0]->complete(200,R"({"pairings":[]})");
        QTRY_COMPARE(net.calls.size(),2);
        net.calls[1]->complete(200,R"({"pairings":[{"id":"0123456789abcdef0123456789abcdef","address":"10.0.0.1"}]})");
        QTRY_COMPARE(net.calls.size(),3);
        QCOMPARE(net.calls[2]->operation(),QNetworkAccessManager::PostOperation);
        net.calls[2]->complete(200,R"({"status":true})");
        QCOMPARE(delivered.count(),1);
        pairing.deliver("10.208.0.9","5678","test","10.0.0.1","test","fixture-only");
        QTRY_COMPARE(net.calls.size(),4);
        net.calls[3]->complete(200,R"({"pairings":[]})");
        pairing.cancel(); QTest::qWait(600); QCOMPARE(net.calls.size(),4);
    }
    void cancellingPairingAbortsOutstandingCredentialRequest() {
        ControlledNetwork net; OmnuvPairing pairing(&net);
        QSignalSpy delivered(&pairing,&OmnuvPairing::delivered);
        QSignalSpy failed(&pairing,&OmnuvPairing::failed);
        pairing.deliver("10.208.0.9","1234","test","","test","fixture-only");
        QTRY_COMPARE(net.calls.size(),1);
        auto reply=net.calls[0]; pairing.cancel();
        QVERIFY(reply->aborted); QCOMPARE(delivered.count(),0); QCOMPARE(failed.count(),0);
        // And cancelled while the name is still resolving: nothing is ever sent.
        pairing.deliver("10.208.0.9","1234","test","","test","fixture-only");
        pairing.cancel(); QTest::qWait(200);
        QCOMPARE(net.calls.size(),1);
    }
    void cancelledStartCannotReappear() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.signIn();
        QTRY_VERIFY(server.find("/v1/auth/device")>=0);
        s.cancelSignIn();
        server.answer(server.find("/v1/auth/device"),200,R"({"device_code":"old","user_code":"OLD","interval":1})");
        QTest::qWait(80);
        QVERIFY(s.userCode().isEmpty()); QVERIFY(!s.busy()); QVERIFY(!s.m_pollTimer.isActive()); QVERIFY(!s.signedIn());
    }
    void cancelledPollCannotInstallTokenAndPollsAreSingleFlight() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.m_deviceCode="old";
        s.poll(); s.poll(); s.poll();
        QTRY_COMPARE(server.count("/v1/auth/device/token"),1);
        s.cancelSignIn();
        server.answer(server.find("/v1/auth/device/token"),200,R"({"token":"discarded-test-token"})");
        QTest::qWait(80);
        QVERIFY(!s.signedIn()); QVERIFY(!s.m_pollPending);
        QCOMPARE(server.count("/v1/auth/device/token"),1);
    }
    // H20: a poll that fails in passing is polled again, and the approval the
    // person already gave is collected; only Core saying no ends it.
    void aPollThatFailsInPassingKeepsWaiting() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.m_deviceCode="approved"; s.m_codeDeadline.setRemainingTime(60000);
        s.poll(); QTRY_COMPARE(server.count("/v1/auth/device/token"),1);
        server.answer(server.find("/v1/auth/device/token"),503,R"({"error":"unavailable"})");
        QTRY_VERIFY(!s.m_pollPending);
        QCOMPARE(s.m_deviceCode,QString("approved"));
        QVERIFY(s.status().contains("still waiting"));
        s.poll(); QTRY_COMPARE(server.count("/v1/auth/device/token"),2);
        server.answer(server.find("/v1/auth/device/token",server.find("/v1/auth/device/token")+1),200,R"({"token":"collected-after-a-503"})");
        QTRY_VERIFY(s.signedIn());
    }
    void aCodePastItsTimeStopsWaiting() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.m_deviceCode="stale"; s.m_codeDeadline.setRemainingTime(0);
        s.poll(); QTRY_COMPARE(server.count("/v1/auth/device/token"),1);
        server.answer(server.find("/v1/auth/device/token"),503,"{}");
        QTRY_VERIFY(s.m_deviceCode.isEmpty());
        QVERIFY(s.status().contains("expired"));
    }
    // H23: refused, expired and used are three answers; Core's words say which.
    void aRefusalSaysCoresReason() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.m_deviceCode="used"; s.m_codeDeadline.setRemainingTime(60000);
        s.poll(); QTRY_COMPARE(server.count("/v1/auth/device/token"),1);
        server.answer(server.find("/v1/auth/device/token"),400,R"({"error":"that code has already been used"})");
        QTRY_VERIFY(s.m_deviceCode.isEmpty());
        QVERIFY2(s.status().contains("already been used"), qPrintable(s.status()));
    }
    void supersededStartCannotOverwriteNewCode() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s;
        s.signOut(); s.signIn(); QTRY_VERIFY(server.find("/v1/auth/device")>=0);
        int first=server.find("/v1/auth/device");
        s.signIn(); QTRY_VERIFY(server.find("/v1/auth/device",first+1)>=0);
        server.answer(server.find("/v1/auth/device",first+1),200,R"({"device_code":"new","user_code":"NEW","interval":60})");
        QTRY_COMPARE(s.userCode(),QString("NEW"));
        server.answer(first,200,R"({"device_code":"old","user_code":"OLD","interval":1})");
        QTest::qWait(80); QCOMPARE(s.userCode(),QString("NEW"));
    }
    void machineRepliesFromPreviousProjectAreIgnored_data() {
        QTest::addColumn<int>("status"); QTest::newRow("late success")<<200; QTest::newRow("late revocation")<<401;
    }
    void machineRepliesFromPreviousProjectAreIgnored() {
        QFETCH(int,status); HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.refresh(); QTRY_VERIFY(server.find("/v1/instances?project=a")>=0);
        int old=server.find("/v1/instances?project=a");
        s.selectProject("b"); QTRY_VERIFY(server.find("/v1/instances?project=b")>=0);
        server.answer(server.find("/v1/instances?project=b"),200,QJsonDocument(QJsonArray{machine("b","b.internal")}).toJson());
        QTRY_COMPARE(s.machines()->idAt(0),QString("b"));
        server.answer(old,status,QJsonDocument(QJsonArray{machine("a","a.internal")}).toJson());
        QTest::qWait(80); QCOMPARE(s.projectId(),QString("b")); QCOMPARE(s.machines()->idAt(0),QString("b")); QVERIFY(s.signedIn());
    }
    // BUYER-13: signing out takes this device off the buyer's network, at Core
    // (while the token still works) and locally; access taken back stops it
    // locally without asking a Core that no longer accepts the token.
    void signingOutTakesThisDeviceOffItsNetwork() {
        // A real session against the held server rather than a fixture one: a
        // fixture session sends nothing to Core on sign-out, by design.
        HeldServer server; qunsetenv("OMNUV_FIXTURE_URL"); OmnuvSession s; s.setCoreUrl(QString::fromUtf8(server.url())); prepare(s);
        FixtureTunnel daemon; daemon.hasIdentity=daemon.verified=true; daemon.state=2; daemon.revision="joined";
        // Stated in full: a real session's scope depends on sign-in state this
        // test does not build, and the tunnel refuses a membership missing any of
        // these five — rightly, since it would not know which one it is stopping.
        daemon.membership=QJsonObject{{"core_url",QString::fromUtf8(server.url())},{"account_id","account-a"},
            {"project_id","a"},{"network_id","network-a"},{"device_id","device-a"}};
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->check(); QCOMPARE(s.m_tunnel->membership(),daemon.membership);
        s.m_token="fixture-token";
        s.signOut();
        QVERIFY2(daemon.commands.contains("stop-v1"),"the tunnel was left on the buyer's network after sign-out");
        QTRY_VERIFY2(server.find("/v1/networks/network-a/devices/device-a")>=0,"Core was not asked to remove this device");
        QVERIFY(server.find("/v1/devices/tokens/current")>=0);
    }
    void accessTakenBackStopsTheMembershipWithoutAskingCore() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        FixtureTunnel daemon; daemon.hasIdentity=daemon.verified=true; daemon.state=2; daemon.revision="joined";
        daemon.membership=s.networkScope(); daemon.membership["network_id"]="network-a"; daemon.membership["device_id"]="device-a";
        s.m_tunnel->m_request=[&](const QString& line) { return daemon.answer(line); };
        s.m_tunnel->check();
        s.m_token="dead-token"; s.accessTakenBack();
        QVERIFY2(daemon.commands.contains("stop-v1"),"a device whose access was taken back stayed on the network");
        QTest::qWait(80);
        QCOMPARE(server.find("/v1/networks/network-a/devices/device-a"),-1);
    }
    // BUYER-12: the machine's admin login goes only to an address on the
    // project network, and a name that resolves anywhere else is refused before
    // any request is made.
    void pairingLoginGoesOnlyToAnOverlayAddress() {
        QCOMPARE(OmnuvPairing::overlayAddress({QHostAddress("10.208.0.7")}),QString("10.208.0.7"));
        QCOMPARE(OmnuvPairing::overlayAddress({QHostAddress("192.168.1.9"),QHostAddress("10.215.255.254")}),QString("10.215.255.254"));
        for (const auto* outside : {"10.207.255.255","10.216.0.0","127.0.0.1","192.168.100.85","100.64.0.1"})
            QVERIFY2(OmnuvPairing::overlayAddress({QHostAddress(outside)}).isEmpty(),outside);
        QVERIFY(OmnuvPairing::overlayAddress({}).isEmpty());
    }
    void pairingToANameOffTheOverlaySendsNothing() {
        // A network that records every request, so "nothing was sent" is a
        // count of what the pairing itself asked, not of a server it would
        // never have reached.
        ControlledNetwork net; OmnuvPairing pairing(&net);
        QSignalSpy failed(&pairing,&OmnuvPairing::failed);
        // Resolves, and resolves to loopback — off the overlay.
        pairing.deliver("localhost","1234","test-client","10.208.0.9","sunshine","admin-secret");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(),1,5000);
        QVERIFY(failed.first().at(0).toString().contains("private network"));
        QTest::qWait(100);
        QCOMPARE(net.calls.size(),0);
    }
    void identityReplyCannotRestoreSignedOutSession_data() {
        QTest::addColumn<int>("status"); QTest::newRow("success")<<200; QTest::newRow("revocation")<<401;
    }
    void identityReplyCannotRestoreSignedOutSession() {
        QFETCH(int,status); HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.fetchIdentity(); QTRY_VERIFY(server.find("/v1/me")>=0);
        s.signOut(); s.m_token="new-session"; // a later successful sign-in
        server.answer(server.find("/v1/me"),status,R"({"email":"old@test.invalid","projects":[{"id":"old","name":"Old"}]})");
        QTest::qWait(80); QVERIFY(s.accountEmail().isEmpty()); QVERIFY(s.projectIds().isEmpty()); QCOMPARE(s.m_token,QString("new-session"));
    }
    void targetsSurviveReorderButNotReplacementOrContextChange() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        auto a=machine("a","a.internal"),b=machine("b","b.internal");
        s.machines()->replace({a,b}); auto target=s.connectionTarget(0); QVERIFY(!target.isEmpty());
        s.machines()->replace({b,a}); QCOMPARE(s.targetRow(target),1);
        a["private_name"]="changed.internal"; s.machines()->replace({b,a}); QCOMPARE(s.targetRow(target),-1);
        a["private_name"]="a.internal"; s.machines()->replace({a,b}); QCOMPARE(s.targetRow(target),0);
        s.signOut(); s.m_token="different-account"; QCOMPARE(s.targetRow(target),-1);
    }
    void cancelledDeploymentLookupCannotCollectCredentials() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.machines()->replace({machine("a","a.internal")}); auto target=s.connectionTarget(0);
        s.deliverPin(target,"1234"); QTRY_VERIFY(server.find("/v1/deployments?project=a")>=0);
        s.selectProject("b");
        server.answer(server.find("/v1/deployments?project=a"),200,R"([{"id":"d","instance_id":"a"}])");
        QTest::qWait(80); QCOMPARE(server.find("/v1/deployments/d/stream-credentials/claim"),-1); QVERIFY(!s.m_pairScope);
    }
    void lostClaimReplyRetriesSameAttemptAndCancellationClosesIt() {
        HeldServer server; qputenv("OMNUV_FIXTURE_URL",server.url()); OmnuvSession s; prepare(s);
        s.machines()->replace({machine("a","a.internal")}); auto target=s.connectionTarget(0);
        s.deliverPin(target,"1234"); QTRY_VERIFY(server.find("/v1/deployments?project=a")>=0);
        server.answer(server.find("/v1/deployments?project=a"),200,R"([{"id":"d","instance_id":"a"}])");
        const QByteArray path="/v1/deployments/d/stream-credentials/claim";
        QTRY_VERIFY(server.find(path)>=0); int first=server.find(path);
        server.answer(first,503,"{}");
        QTRY_VERIFY_WITH_TIMEOUT(server.find(path,first+1)>=0,7000);
        int second=server.find(path,first+1); QCOMPARE(server.calls[first].body,server.calls[second].body);
        s.finishPairing(); QTRY_VERIFY(server.find("/v1/deployments/d/stream-credentials/complete")>=0);
        // The same attempt closes, and says it was not delivered: since BUYER-11
        // (22 September) a pairing that did not work gives the login back rather
        // than spending it. This compared the whole body with the claim's, which
        // predates `delivered`.
        const auto claimed=QJsonDocument::fromJson(server.calls[first].body).object();
        const auto completed=QJsonDocument::fromJson(server.calls[server.find("/v1/deployments/d/stream-credentials/complete")].body).object();
        QCOMPARE(completed.value("attempt_id"),claimed.value("attempt_id"));
        QCOMPARE(completed.value("delivered"),QJsonValue(false));
        QVERIFY(!s.m_pairScope);
    }
};

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    QTemporaryDir settings;
    QSettings::setPath(QSettings::NativeFormat,QSettings::UserScope,settings.path());
    qputenv("ONV_TUNNEL_DIR",settings.path().toUtf8());
    OmnuvSessionTest test; return QTest::qExec(&test,argc,argv);
}
#include "session_test.moc"
