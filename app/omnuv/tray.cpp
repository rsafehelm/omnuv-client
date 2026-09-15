// **Before every include, because it decides which declarations exist.**
//
// `SHQueryUserNotificationState` is gated on `NTDDI_VERSION >= 0x06000000` in
// `shellapi.h`, and two of the states it returns are gated higher still —
// `QUNS_QUIET_TIME` on Windows 7 and `QUNS_APP` on Windows 8. Those are enum
// constants rather than macros, so a `#ifdef` around them is not available:
// either the translation unit declares a modern enough SDK or the switch below
// does not compile. Windows SDK headers derive `NTDDI_VERSION` from
// `_WIN32_WINNT` when it is not set, and the default differs by toolchain, so
// this file states it instead of inheriting whatever the compiler felt like.
// 0x0A00 is Windows 10, which is this fork's floor everywhere else too.
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif

#include "tray.h"
#include "autostart.h"
#include "appearance.h"
#include "machinemodel.h"
#include "omnuvsession.h"
#include "probe.h"
#include "tunnel.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSignalBlocker>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
// Qt first, <windows.h> last: it defines `min` and `max` as macros, and a Qt
// header parsed after it is how that becomes somebody else's afternoon.
#include <windows.h>
#include <shellapi.h>
#endif

// A minute, asked once a second. Long enough for a shell still assembling the
// notification area during a login, short enough that whatever reads the log
// is not kept waiting on a machine that simply has no tray.
static constexpr int kRegisterPollMs = 1000;
static constexpr int kRegisterWaits = 60;

// **The tray takes its own local readings, and that is not a duplicate of the
// window's poll.** `OmnuvTunnel::watch()` is turned on by `OmnuvView` when it
// becomes the current item of the StackView and off when it stops being one —
// so it is running while the window is merely hidden, and *stopped* the whole
// time a stream is on screen, which is exactly when somebody is most likely to
// glance at the tray. The reading would then be frozen at whatever it said
// when they pressed Play, with nothing on the menu admitting it.
//
// One minute, matching `kRefreshHiddenMs` in `omnuvsession.cpp`, for the same
// reason the operator gave there: this is about a person watching something
// come up, not about being current to the second. Both readings also carry
// their age in the menu, which is the half that makes a frozen one visible
// rather than merely less likely.
static constexpr int kLocalPollMs = 60 * 1000;

// The connecting ring: eight frames, an eighth of a turn each, one revolution a
// second. **Not one of `Theme.qml`'s durations, deliberately** — those are
// Windows' own motion table for a control transition, and the slowest of them
// is 333ms, which is a transition rather than a rotation. This is a period and
// Windows publishes none, so it is chosen rather than sourced, and it is the
// one number here worth tuning on the rig: too fast reads as agitated, too slow
// reads as stuck, and eight `Shell_NotifyIcon` calls a second is the cost.
// ponytail: chosen by eye; if it reads wrong on hardware, this constant is the
// whole knob.
static constexpr int kSpinFrames = 8;
static constexpr int kSpinMs = 1000 / kSpinFrames;

// The mark, in colour, for a notification's large icon — which is what Windows
// draws from `NIIF_USER | NIIF_LARGE_ICON`, at a size where four blocks read.
// The *tray* icon is painted rather than loaded: see `glyph()`.
static const char* kMarkPath = ":/omnuv/omnuv.svg";

namespace {

// **Microsoft's four system fill colours, transcribed a second time, and the
// build fails if this table and `Theme.qml`'s stop agreeing.**
//
// A duplicate, and it is deliberate rather than an oversight. `Theme.qml` is a
// QML singleton: reaching it from C++ means the engine, a uri and a type name
// resolved by string at a moment when the tray has to work whether or not any
// of that succeeded. The repository's own answer to a vocabulary that must not
// drift across a boundary it cannot share is a pin — the launch ladder is
// pinned against the web console's the same way — so
// `.github/workflows/omnuv-change-budget.yml` extracts these six pairs from
// `Theme.qml` and asserts they appear here.
//
// Light and dark are the *taskbar's*, not the window's: see
// `OmnuvAppearance::darkSystemTheme`.
QColor fillSuccess(bool dark)  { return dark ? QColor("#6CCB5F") : QColor("#0F7B0F"); }
QColor fillCaution(bool dark)  { return dark ? QColor("#FCE100") : QColor("#9D5D00"); }
QColor fillCritical(bool dark) { return dark ? QColor("#FF99A4") : QColor("#C42B1C"); }
QColor fillNeutral(bool dark)  { return dark ? QColor("#8BFFFFFF") : QColor("#72000000"); }

// One dot for a menu row. **Filled or hollow is the load-bearing half of it**,
// not the colour: Microsoft sets all four system fill colours to the same red
// under a high-contrast theme, on purpose, because colour stops carrying
// meaning there. So an unknown reading is a ring rather than a dimmer disc,
// and every row draws its word beside the dot regardless.
QIcon dot(const QColor& colour, bool filled)
{
    static constexpr int kSide = 16;
    static constexpr qreal kDiameter = 9.0;
    static constexpr qreal kStroke = 1.6;

    QPixmap pm(kSide, kSide);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF circle((kSide - kDiameter) / 2.0, (kSide - kDiameter) / 2.0,
                        kDiameter, kDiameter);
    if (filled) {
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawEllipse(circle);
    }
    else {
        p.setPen(QPen(colour, kStroke));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle.adjusted(kStroke / 2, kStroke / 2, -kStroke / 2, -kStroke / 2));
    }
    return QIcon(pm);
}

// How long ago a reading was taken, in a person's words.
//
// **Empty for an invalid stamp, never "just now".** A reading that has never
// been taken and one taken a second ago are the two things this whole
// vocabulary exists to keep apart, and dating a claim nobody made is how a
// stale dot looks fresh.
QString age(const QDateTime& at)
{
    if (!at.isValid()) {
        return QString();
    }
    const qint64 s = at.secsTo(QDateTime::currentDateTime());
    if (s < 0)   { return QString(); }          // a clock that moved backwards
    if (s < 10)  { return QObject::tr("just now"); }
    if (s < 90)  { return QObject::tr("%1 s ago").arg(s); }
    return QObject::tr("%1 min ago").arg(s / 60);
}

QString word(omnuv::Reading r)
{
    switch (r) {
    case omnuv::Reading::Pass: return QObject::tr("pass");
    case omnuv::Reading::Fail: return QObject::tr("fail");
    case omnuv::Reading::Unknown: break;
    }
    return QObject::tr("unknown");
}

const char* iconName(omnuv::TrayIcon i)
{
    switch (i) {
    case omnuv::TrayIcon::Connected: return "connected";
    case omnuv::TrayIcon::Working:   return "working";
    case omnuv::TrayIcon::Attention: return "attention";
    case omnuv::TrayIcon::Idle:      break;
    }
    return "idle";
}

// What the shell was willing to show, at the moment we asked it to show one.
//
// **`QSystemTrayIcon::showMessage` cannot report a refusal, in three separate
// places**, all of them read in Qt's own source rather than assumed:
// `QWindowsSystemTrayIcon::showMessage` discards `Shell_NotifyIcon`'s return
// value where the same file checks it twenty lines later; `supportsMessages()`
// is one Windows 7-era registry value that normally does not exist and
// defaults to true; and `QSystemTrayIcon::showMessage` is gated on Qt's own
// `visible` flag rather than on the shell's answer. Qt's class documentation
// says it plainly — *"messages may not appear at all"*.
//
// So a toast that was suppressed and a change that never happened leave the
// same trace, which is the watcher failure exactly. This is the cheapest thing
// that tells them apart: one `shell32` call, no dependency, no AUMID, logged
// beside every attempt. Tokens are the constant's own name, so a reader can
// look the state up rather than trust this comment.
const char* shellNotificationState()
{
#ifdef Q_OS_WIN
    QUERY_USER_NOTIFICATION_STATE state;
    if (FAILED(SHQueryUserNotificationState(&state))) {
        return "unreadable";
    }
    switch (state) {
    case QUNS_NOT_PRESENT:             return "quns-not-present";
    case QUNS_BUSY:                    return "quns-busy";
    case QUNS_RUNNING_D3D_FULL_SCREEN: return "quns-d3d-full-screen";
    case QUNS_PRESENTATION_MODE:       return "quns-presentation-mode";
    case QUNS_ACCEPTS_NOTIFICATIONS:   return "quns-accepts-notifications";
    case QUNS_QUIET_TIME:              return "quns-quiet-time";
    case QUNS_APP:                     return "quns-app";
    }
    return "unrecognised";
#else
    // No equivalent, and saying so is better than an answer of "fine" that
    // would make a Linux log look like a Windows one that had been checked.
    return "unasked";
#endif
}

} // namespace

OmnuvTray* OmnuvTray::create(OmnuvSession* session, QObject* parent)
{
    return new OmnuvTray(session, parent);
}

OmnuvTray::OmnuvTray(OmnuvSession* session, QObject* parent)
    : QObject(parent),
      m_session(session),
      m_probe(new OmnuvProbe(this)),
      m_icon(new QSystemTrayIcon(this)),
      m_menu(new QMenu()),
      m_state(nullptr),
      m_machinesHeader(nullptr),
      m_deviceAnchor(nullptr),
      m_rowInternet(nullptr),
      m_rowOmnuv(nullptr),
      m_rowNetwork(nullptr),
      m_autostart(nullptr),
      // The session's instance, not a second one: two objects writing the same
      // registry key would disagree about what is true.
      m_auto(session != nullptr ? session->autostart() : nullptr),
      m_localPoll(new QTimer(this)),
      m_spinner(new QTimer(this)),
      m_frame(0),
      m_painted(omnuv::TrayIcon::Idle),
      m_paintedDark(true),
      m_paintedFrame(0),
      m_everPainted(false),
      m_registerWatch(new QTimer(this)),
      m_registerWaitsLeft(kRegisterWaits)
{
    QAction* open = m_menu->addAction(tr("Open Omnuv"));
    connect(open, &QAction::triggered, this, &OmnuvTray::openWindow);

    m_menu->addSeparator();

    // The headline: Omnuv's own state in one line, which is what this menu
    // showed and all it showed until today.
    m_state = m_menu->addAction(QString());
    m_state->setEnabled(false);

    m_menu->addSeparator();

    // **Machines**, rebuilt on every open. The header carries the count so an
    // empty estate says so rather than leaving a gap that reads as a failure
    // to load.
    m_machinesHeader = m_menu->addAction(QString());
    m_machinesHeader->setEnabled(false);

    // Rows are inserted immediately before this separator, which is therefore
    // the anchor and never moves.
    m_deviceAnchor = m_menu->addSeparator();

    QAction* deviceHeader = m_menu->addAction(tr("This device"));
    deviceHeader->setEnabled(false);

    // The three local readings. Disabled because they are readings: there is
    // nothing to press, and a row that highlights under the cursor promises
    // otherwise.
    m_rowInternet = m_menu->addAction(QString());
    m_rowInternet->setEnabled(false);
    m_rowOmnuv = m_menu->addAction(QString());
    m_rowOmnuv->setEnabled(false);
    m_rowNetwork = m_menu->addAction(QString());
    m_rowNetwork->setEnabled(false);

    m_menu->addSeparator();

    // **Start with Windows, and it reflects the machine rather than us.**
    // The checkbox is set from what the Run key actually says each time the
    // menu is built, so a person who turned it off in Settings sees it off
    // here. A switch that reports our intention instead of the system's state
    // is a switch that lies.
    //
    // Hidden entirely where there is no implementation, rather than shown
    // greyed: an inert control invites the question "why can I not use this".
    if (m_auto != nullptr && m_auto->supported()) {
        m_autostart = m_menu->addAction(tr("Start with %1").arg(
#if defined(Q_OS_WIN)
            tr("Windows")
#elif defined(Q_OS_DARWIN)
            tr("macOS")
#else
            tr("this computer")
#endif
        ));
        m_autostart->setCheckable(true);
        connect(m_autostart, &QAction::toggled, this, &OmnuvTray::toggleAutostart);
        m_menu->addSeparator();
    }

    QAction* about = m_menu->addAction(tr("About Omnuv Connect"));
    connect(about, &QAction::triggered, this, &OmnuvTray::showAbout);

    QAction* quit = m_menu->addAction(tr("Quit Omnuv"));
    // The only thing that actually quits. Closing the window hides it; a
    // widget that cannot be quit from its own tray is one people uninstall.
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    // **Unconditional, and it used to be nested inside the autostart branch.**
    // On a platform where autostart is unimplemented the menu was then never
    // refreshed on open — it moved only when a signal happened to fire — which
    // was survivable for one static line and is not for a menu with rows in
    // it and ages beside them.
    connect(m_menu, &QMenu::aboutToShow, this, &OmnuvTray::refresh);

    m_icon->setContextMenu(m_menu);
    connect(m_icon, &QSystemTrayIcon::activated, this, &OmnuvTray::activated);

    if (m_session != nullptr) {
        connect(m_session, &OmnuvSession::signedInChanged, this, &OmnuvTray::refresh);
        connect(m_session, &OmnuvSession::coreUrlChanged, this, [this]() {
            m_probe->setCoreUrl(m_session->coreUrl());
            m_probe->check();
        });
        if (m_session->tunnel() != nullptr) {
            connect(m_session->tunnel(), &OmnuvTunnel::changed, this, &OmnuvTray::refresh);
        }
        if (m_session->appearance() != nullptr) {
            // The taskbar's theme can change under us, and a monochrome glyph
            // painted for the wrong one is invisible rather than merely wrong.
            connect(m_session->appearance(), &OmnuvAppearance::changed,
                    this, &OmnuvTray::refresh);
        }
        if (m_session->machines() != nullptr) {
            connect(m_session->machines(), &MachineModel::countChanged,
                    this, &OmnuvTray::refresh);
        }
        m_probe->setCoreUrl(m_session->coreUrl());
    }
    connect(m_probe, &OmnuvProbe::changed, this, &OmnuvTray::refresh);

    // **Three notifications, and every one of them is a transition.**
    //
    // The model decides which; see the signal list in `machinemodel.h` for why
    // it is these three and not the other four that suggest themselves. The
    // detail each one carries is Core's own sentence, verbatim — a `last_error`
    // or a `waiting_on`. Nothing here composes a cause, and nothing here
    // mentions a provider, a hypervisor or a number.
    if (m_session != nullptr && m_session->machines() != nullptr) {
        MachineModel* machines = m_session->machines();

        connect(machines, &MachineModel::machineBecameReady, this, [this](const QString& name) {
            toast("machine-ready", tr("%1 is ready").arg(name),
                  tr("It finished starting and you can connect to it."));
        });

        connect(machines, &MachineModel::machineNeedsAttention,
                this, [this](const QString& name, const QString& why) {
                    toast("machine-lost", tr("%1 needs attention").arg(name),
                          why.isEmpty() ? tr("It was running and it is not any more.") : why);
                });

        connect(machines, &MachineModel::machineIsWaiting,
                this, [this](const QString& name, const QString& waitingOn) {
                    // `waiting_on` is already in the buyer's terms — it is what
                    // the console shows — so it is passed through rather than
                    // rewritten. The reassurance is ours and is true: nothing
                    // is charged for a request that is waiting.
                    toast("machine-waiting", tr("%1 is waiting").arg(name),
                          tr("%1 · nothing is charged while it waits.").arg(waitingOn));
                });
    }

    m_spinner->setInterval(kSpinMs);
    connect(m_spinner, &QTimer::timeout, this, &OmnuvTray::spin);

    m_localPoll->setInterval(kLocalPollMs);
    connect(m_localPoll, &QTimer::timeout, this, &OmnuvTray::probeLocal);
    m_localPoll->start();
    probeLocal();

    refresh();

    // Unconditional. A headless session, a desktop with no StatusNotifier
    // host, a kiosk — all real, and all answered by Qt adding the entry if
    // and when an area appears, rather than by us deciding now. The open
    // question after this call is *when* the shell took it, never *whether*
    // we asked.
    m_icon->show();

    m_registerWatch->setInterval(kRegisterPollMs);
    connect(m_registerWatch, &QTimer::timeout, this, &OmnuvTray::checkRegistered);
    checkRegistered();
}

// The one line anything watching this program reads, and the honest moment to
// write it.
//
// `show()` above is the call that registers with the shell, so nothing before
// it can claim anything: a constructor that ran proves this process built some
// objects, which is true on a machine with no shell at all. But `show()`
// returns void, and the `visible` property is our own flag read back — Qt's
// documentation says setting it *"makes the system tray icon visible"*, not
// that the shell accepted it — so asking the icon whether it worked is asking
// the one object that cannot know. `isSystemTrayAvailable()` is the only
// question in this API whose answer comes from outside the process, so that is
// the question, asked in the same breath as `show()`.
//
// When it says no we wait instead of concluding, because Qt documents that it
// adds the entry itself once an area appears, and a notification area that is
// not up yet is the normal state for a program that starts at login. There is
// no signal for it, so this polls: a minute, then a verdict.
//
// **Exactly one `tray=` line per run, and there is always one.** A log with
// neither line is a client that never reached here — a different fault from a
// tray that could not register, and the two must not look alike. printf-style
// rather than `qInfo() <<` so the token is the entire message, with no QDebug
// quoting or trailing space between it and the newline.
//
// What `tray=ready` does not claim: that anyone can see the icon. Windows 11
// files new icons into the overflow flyout by default, and that is a shell
// preference rather than a failure — this says the icon is registered, which
// is the fact a harness can act on.
void OmnuvTray::checkRegistered()
{
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_registerWatch->stop();
        qInfo("tray=ready");
    }
    else if (m_registerWaitsLeft-- > 0) {
        m_registerWatch->start();
    }
    else {
        // Named rather than bare, so a later cause — a platform with no tray
        // at all, a refusal we learn to detect — arrives as a new reason
        // instead of changing what this one means.
        m_registerWatch->stop();
        qInfo("tray=unavailable:no-notification-area");
    }
}

void OmnuvTray::probeLocal()
{
    m_probe->check();
    if (m_session != nullptr && m_session->tunnel() != nullptr) {
        m_session->tunnel()->check();
    }
}

omnuv::Readings OmnuvTray::readings() const
{
    omnuv::Readings r;
    if (m_session == nullptr) {
        return r;
    }

    r.signedIn = m_session->signedIn();

    OmnuvTunnel* tunnel = m_session->tunnel();
    if (tunnel != nullptr) {
        r.tunnelInstalled = tunnel->available();
        r.joining = tunnel->busy();
        r.network = tunnel->reading();
    }

    MachineModel* machines = m_session->machines();
    if (machines != nullptr) {
        r.machinesMoving = machines->movingCount();
        r.machinesUnhappy = machines->unhappyCount();
    }

    r.internet = m_probe->internet();
    r.omnuv = m_probe->core();
    return r;
}

QString OmnuvTray::stateLine() const
{
    if (m_session == nullptr || !m_session->signedIn()) {
        return tr("Not signed in");
    }

    OmnuvTunnel* tunnel = m_session->tunnel();
    if (tunnel == nullptr || !tunnel->available()) {
        return tr("Network client not installed");
    }
    if (tunnel->connected()) {
        return tr("Network: %1").arg(tunnel->address());
    }
    // Says what is true, including when the answer is that we cannot tell. A
    // tray reading "Connected" because nothing has contradicted it yet is
    // worse than one that admits it does not know.
    return tr("Network: %1").arg(tunnel->state());
}

void OmnuvTray::setRow(QAction* row, const QString& label, omnuv::Reading reading,
                       const QDateTime& takenAt, const QString& replacement)
{
    const bool dark = m_session != nullptr && m_session->appearance() != nullptr
                          ? m_session->appearance()->darkSystemTheme()
                          : true;

    // A replacement covers the one case that is neither a pass, a failure nor
    // an unknown: something the person has not installed yet. That is a
    // pending action, and "an action the person has not taken yet is not a
    // failure" — so it gets a neutral hollow dot and its own word, rather than
    // a red one that teaches people to ignore red.
    if (!replacement.isEmpty()) {
        row->setIcon(dot(fillNeutral(dark), false));
        row->setText(tr("%1 · %2").arg(label, replacement));
        return;
    }

    switch (reading) {
    case omnuv::Reading::Pass:
        row->setIcon(dot(fillSuccess(dark), true));
        break;
    case omnuv::Reading::Fail:
        row->setIcon(dot(fillCritical(dark), true));
        break;
    case omnuv::Reading::Unknown:
        row->setIcon(dot(fillNeutral(dark), false));
        break;
    }

    const QString when = age(takenAt);
    row->setText(when.isEmpty() ? tr("%1 · %2").arg(label, word(reading))
                                : tr("%1 · %2 · %3").arg(label, word(reading), when));
}

void OmnuvTray::rebuildMachines()
{
    for (QAction* row : m_machineRows) {
        m_menu->removeAction(row);
        row->deleteLater();
    }
    m_machineRows.clear();

    MachineModel* machines = m_session != nullptr ? m_session->machines() : nullptr;
    const int count = machines != nullptr ? machines->rowCount() : 0;

    // Hidden rather than shown empty when nobody is signed in: a heading over
    // nothing reads as a list that failed to load, and the headline two rows
    // up has already said what is actually true.
    m_machinesHeader->setVisible(m_session != nullptr && m_session->signedIn());
    if (m_session == nullptr || !m_session->signedIn()) {
        return;
    }
    m_machinesHeader->setText(count == 0 ? tr("Machines — none yet") : tr("Machines"));

    const bool dark = m_session->appearance() != nullptr
                          ? m_session->appearance()->darkSystemTheme()
                          : true;

    for (int i = 0; i < count; ++i) {
        const QModelIndex idx = machines->index(i, 0);
        const QString name = machines->data(idx, MachineModel::NameRole).toString();
        const QString status = machines->data(idx, MachineModel::StatusRole).toString();

        // **The dot is keyed on Core's word, through the model's own grouping,
        // and nothing else.** No local judgement, no second vocabulary, and no
        // attempt to say why — the card in the window carries `last_error`,
        // the ladder and the elapsed time, and this row carries what fits on
        // one line.
        QColor colour = fillCritical(dark);
        switch (machines->healthAt(i)) {
        case Machine::Health::Good:    colour = fillSuccess(dark); break;
        case Machine::Health::Moving:  colour = fillCaution(dark); break;
        case Machine::Health::Resting: colour = fillNeutral(dark); break;
        case Machine::Health::Bad:     break;
        }

        // **The dot is solid whatever the observation says, and the words
        // carry the doubt instead.** The temptation is to draw an unobserved
        // machine hollow, the way the device rows draw an unknown reading —
        // and it is the wrong instinct here, because the card in the window
        // does not do that, and one machine wearing two different dots in two
        // of our own surfaces is the drift this whole section refuses.
        //
        // So the card's own sentence comes along instead. It has three
        // branches and this has two: a sweep that was incomplete is folded in
        // with one that never happened, because both mean the same thing to
        // somebody reading a menu — nobody has confirmed this lately — and a
        // tray row has no room for the difference. The card keeps it.
        const bool observed = machines->data(idx, MachineModel::ObservedRole).toBool()
                              && machines->data(idx, MachineModel::ObservationCompleteRole).toBool();
        // An observation whose timestamp did not parse is an observation we
        // cannot date, and `age()` returns nothing for it. "observed " with a
        // hole where the time goes is worse than saying we do not know.
        const QString when =
            observed ? age(machines->data(idx, MachineModel::ObservedAtRole).toDateTime())
                     : QString();
        const QString seen = when.isEmpty() ? tr("not observed") : tr("observed %1").arg(when);

        QAction* row = new QAction(dot(colour, true),
                                   tr("%1 · %2 · %3").arg(name, status, seen), m_menu);

        // **Opens the window rather than connecting, and that is a deliberate
        // narrowing of D1's "click to connect".** Connecting runs a five-rung
        // segue and, on an unpaired machine, a pairing exchange — both of which
        // draw their progress in the window. Starting that from a menu would
        // put a stream behind a user interface nobody can see, and a failure
        // behind one nobody can read. The window is one click away and it is
        // where Play lives.
        connect(row, &QAction::triggered, this, &OmnuvTray::openWindow);

        m_menu->insertAction(m_deviceAnchor, row);
        m_machineRows.append(row);
    }
}

void OmnuvTray::refresh()
{
    const QString line = stateLine();
    if (m_state != nullptr) {
        m_state->setText(line);
    }
    m_icon->setToolTip(tr("Omnuv — %1").arg(line));

    // Not while it is open. A row appearing or vanishing under a cursor that
    // is already on it is how a person clicks the thing that was there a
    // moment ago, and `aboutToShow` means every open starts from a rebuild
    // anyway.
    if (!m_menu->isVisible()) {
        rebuildMachines();
    }

    OmnuvTunnel* tunnel = m_session != nullptr ? m_session->tunnel() : nullptr;

    setRow(m_rowInternet, tr("Internet"), m_probe->internet(), m_probe->takenAt(), QString());
    setRow(m_rowOmnuv, tr("Omnuv"), m_probe->core(), m_probe->takenAt(), QString());
    setRow(m_rowNetwork, tr("Your network"),
           tunnel != nullptr ? tunnel->reading() : omnuv::Reading::Unknown,
           tunnel != nullptr ? tunnel->takenAt() : QDateTime(),
           tunnel != nullptr && !tunnel->available() ? tr("not installed") : QString());

    if (m_autostart != nullptr) {
        // Set without re-entering the toggle handler, which would write the
        // value back on every menu open.
        QSignalBlocker block(m_autostart);
        m_autostart->setChecked(m_auto != nullptr && m_auto->enabled());
    }

    applyIcon();
}

void OmnuvTray::applyIcon()
{
    const omnuv::TrayIcon want = omnuv::iconFor(readings());
    const bool dark = m_session != nullptr && m_session->appearance() != nullptr
                          ? m_session->appearance()->darkSystemTheme()
                          : true;
    const bool motion = m_session != nullptr && m_session->appearance() != nullptr
                            ? m_session->appearance()->animationsEnabled()
                            : true;

    if (!m_everPainted || want != m_painted) {
        // The evidence that the four states are four. A rig that never sees
        // anything but `trayicon=idle` has found something.
        qInfo("trayicon=%s", iconName(want));
    }

    // **The one animation in this application that does not collapse to zero,
    // and the one that is a different glyph instead.** `Theme.qml` says why:
    // every other duration is an escort and arrives at the same end state when
    // it is removed, but a ring that is not turning is the answer "no" to the
    // question it exists to answer. So reduced motion gets the connecting
    // glyph held at its first frame — the arc is still there, the shape still
    // says *working*, and nothing moves.
    //
    // It stops the moment the state resolves, which is the other half: a ring
    // still turning over a machine that came up five minutes ago is a widget
    // lying about being busy.
    if (want == omnuv::TrayIcon::Working && motion) {
        if (!m_spinner->isActive()) {
            m_spinner->start();
        }
    }
    else {
        m_spinner->stop();
        m_frame = 0;
    }

    if (m_everPainted && want == m_painted && dark == m_paintedDark
        && m_frame == m_paintedFrame) {
        return;
    }
    m_painted = want;
    m_paintedDark = dark;
    m_paintedFrame = m_frame;
    m_everPainted = true;
    m_icon->setIcon(glyph(want, m_frame));
}

void OmnuvTray::spin()
{
    if (m_painted != omnuv::TrayIcon::Working) {
        m_spinner->stop();
        return;
    }
    m_frame = (m_frame + 1) % kSpinFrames;
    m_paintedFrame = m_frame;
    m_icon->setIcon(glyph(m_painted, m_frame));
}

// **Painted rather than loaded, and the reason is the animation.** Four state
// glyphs plus a turning ring is four SVGs and eight frames as files; it is one
// function here, it scales to whatever size the shell asks for instead of to
// the sizes somebody exported, and it takes its ink from the taskbar's own
// theme at the moment of painting rather than from a colour baked into a file.
//
// `omnuv.svg` keeps its job: it is the colour mark, and it is what a
// notification's large icon draws, where four blocks are legible and a
// monochrome silhouette would be anonymous.
//
// The silhouette is that mark's outer square. At sixteen pixels the four
// blocks are mush, and a tray icon that cannot be told from the next one along
// is not an icon.
QIcon OmnuvTray::glyph(omnuv::TrayIcon state, int frame) const
{
    const bool dark = m_session != nullptr && m_session->appearance() != nullptr
                          ? m_session->appearance()->darkSystemTheme()
                          : true;

    // Monochrome, matching the taskbar, which is what every first-party icon
    // beside ours does. The accent appears on exactly one state.
    const QColor ink = dark ? QColor(0xFF, 0xFF, 0xFF) : QColor(0x00, 0x00, 0x00);
    const QColor accent = QGuiApplication::palette().color(QPalette::Accent);

    QIcon icon;
    // 100%, 125%, 150%, 200% and 300% of a 16px notification-area icon. Qt
    // picks the nearest and the shell scales what it gets, so an exact match at
    // the common scalings is the difference between a crisp glyph and a blurred
    // one.
    for (int side : { 16, 20, 24, 32, 48 }) {
        QPixmap pm(side, side);
        pm.fill(Qt::transparent);

        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);

        const qreal stroke = qMax(qreal(1.25), side / 9.0);
        const qreal inset = stroke / 2 + side * 0.05;
        const QRectF box(inset, inset, side - inset * 2, side - inset * 2);
        const qreal radius = side * 0.24;

        switch (state) {
        case omnuv::TrayIcon::Connected:
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawRoundedRect(box, radius, radius);
            break;

        case omnuv::TrayIcon::Idle:
            // Hollow, which is the same shape the menu's unknown rows use, and
            // means the same thing: this is claiming nothing.
            p.setPen(QPen(ink, stroke));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, radius, radius);
            break;

        case omnuv::TrayIcon::Attention: {
            // The badge is *cut out* of the mark rather than laid on top of
            // it, so that at sixteen pixels there is a ring of taskbar between
            // the two and the badge reads as a badge instead of as a notch.
            //
            // Anchored to the **pixmap's** corner and not to the mark's. The
            // mark is inset so its own stroke has room; a badge placed
            // relative to that and then grown by its cut-out ring runs past
            // the pixmap edge and is clipped — at sixteen pixels by nearly a
            // fifth of itself, which reads as a flattened side rather than as
            // a dot somebody meant.
            const qreal r = side * 0.40;
            const QRectF badge(side - r, side - r, r, r);

            QPainterPath mark;
            mark.addRoundedRect(box, radius, radius);
            QPainterPath hole;
            hole.addEllipse(badge.adjusted(-stroke, -stroke, stroke, stroke));
            p.fillPath(mark.subtracted(hole), ink);

            p.setPen(Qt::NoPen);
            p.setBrush(accent);
            p.drawEllipse(badge);
            break;
        }

        case omnuv::TrayIcon::Working: {
            // A ring rather than the square, because a ring with a bright arc
            // sweeping it is the shape every desktop already uses for "wait",
            // and a widget is not the place to teach somebody a new one.
            const QRectF ring = box.adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);

            QColor track = ink;
            track.setAlphaF(0.3);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(track, stroke, Qt::SolidLine, Qt::RoundCap));
            p.drawEllipse(ring);

            // Qt's angles are sixteenths of a degree, zero at three o'clock,
            // positive counter-clockwise — read in `QPainter::drawArc`'s own
            // documentation rather than remembered. So a negative span turns
            // the way a clock does, which is the way every progress ring on
            // this desktop turns.
            const int start = (90 - frame * (360 / kSpinFrames)) * 16;
            p.setPen(QPen(ink, stroke, Qt::SolidLine, Qt::RoundCap));
            p.drawArc(ring, start, -100 * 16);
            break;
        }
        }

        icon.addPixmap(pm);
    }
    return icon;
}

void OmnuvTray::toast(const char* kind, const QString& title, const QString& body)
{
    // **Said before it is sent, and it names what the shell would accept.**
    // A toast that was suppressed and a change that never happened are
    // indistinguishable in a log that records only the change, and this is the
    // half of that pair we can actually write down. `area=` rather than
    // `tray=`, deliberately: `omnuv-run.ps1` reads the registration verdict
    // with `Qt Info: tray=(\S+)` and takes the *last* match, and a token that
    // could be confused with it would be a source-check matching its own
    // evidence one layer out.
    //
    // ponytail: `WinToast` through an AUMID — which is what `windows_impl.md`
    // I3 names — is the upgrade, and it buys three things this does not: the
    // application's own name and icon on the banner, actions on it, and
    // `ToastNotifier::Setting`, which *reports* suppression instead of leaving
    // it to be inferred. It costs a vendored WinRT dependency in a GPL fork
    // with a change budget, plus an installer that creates a Start-menu
    // shortcut carrying `System.AppUserModel.ID` — without which a Win32 app's
    // WinRT toasts do not appear at all. `Shell_NotifyIcon` needs no AUMID
    // (read in `qwindowssystemtrayicon.cpp`: the file contains no reference to
    // one) and renders as a banner on Windows 10 and 11 alike. Take the
    // upgrade when a toast needs a button on it; not for the same pixels.
    qInfo("toast=%s area=%s shell=%s", kind,
          QSystemTrayIcon::isSystemTrayAvailable() ? "ready" : "unavailable",
          shellNotificationState());

    // The colour mark, not the monochrome tray glyph: a notification is drawn
    // at a size where the mark is legible, and the glyph is designed for
    // sixteen pixels on a taskbar.
    //
    // Built per call rather than held in a function-local static. There are
    // three of these in a session, so the cost is nothing, and a QIcon in a
    // static outlives QGuiApplication — which is a destruction order Qt does
    // not owe anybody an answer for.
    //
    // `QFile::exists`, not `QIcon::isNull`: a QIcon built from a filename is
    // not "null" merely because nothing has rendered it yet, which is the
    // mistake that made the tray fall back to upstream's wheel every time.
    const QIcon mark(QFile::exists(QString::fromLatin1(kMarkPath))
                         ? QString::fromLatin1(kMarkPath)
                         : QStringLiteral(":/res/moonlight.svg"));
    m_icon->showMessage(title, body, mark);
}

void OmnuvTray::openWindow()
{
    // The application's own window, found rather than held: this object is
    // created before the QML engine builds it, so a pointer captured at
    // construction would be null.
    const auto windows = QGuiApplication::topLevelWindows();
    for (QWindow* w : windows) {
        if (w->isVisible() || w->type() == Qt::Window) {
            w->show();
            w->raise();
            w->requestActivate();
            return;
        }
    }
}

void OmnuvTray::showAbout()
{
    // **Reachable in one click, and it names upstream.** This program
    // auto-starts, so a person who never deliberately launched it is entitled
    // to find out what it is and where its source lives. That is the GPL's
    // requirement and also simply the decent thing.
    QMessageBox::information(
        nullptr,
        tr("About Omnuv Connect"),
        tr("Omnuv Connect is a fork of Moonlight, the open-source game "
           "streaming client, with sign-in to Omnuv and the marketplace "
           "network added.\n\n"
           "Licensed GPL-3.0.\n"
           "Upstream: github.com/moonlight-stream/moonlight-qt\n"
           "This fork, and what was changed: github.com/rsafehelm/omnuv-client"));
}

void OmnuvTray::toggleAutostart(bool on)
{
    if (m_auto != nullptr) { m_auto->setEnabled(on); }
    // Read it back rather than trusting the write: if the registry refused —
    // policy, permissions, a locked-down machine — the menu should show what
    // is true, not what was attempted.
    refresh();
}

void OmnuvTray::activated(QSystemTrayIcon::ActivationReason reason)
{
    // A person who double-clicks means the same as one who clicked.
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        openWindow();
    }
}
