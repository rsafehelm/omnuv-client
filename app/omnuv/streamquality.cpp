#include "streamquality.h"
#include "omnuv/appearance.h"

#include "streaming/session.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QtGlobal>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using namespace OmnuvStreamQuality;

// ---------------------------------------------------------------------------
// The two colours, mirrored from app/omnuv/Theme.qml
//
// Theme is a QML singleton and QML does not run during a stream, so these
// cannot be read from it — they are copied, and
// `.github/workflows/omnuv-change-budget.yml` fails the build when the copy
// and the original disagree. That check is the only thing that makes a mirror
// safe; without it this is a second definition waiting to drift.
//
// The **dark** variant of each, always, and that is not a bug. Theme picks
// between a light and a dark value by the apps theme, because its consumers
// sit on a window surface. These sit on video, which has no theme, over a 4px
// black outline that `OverlayManager::RenderTextOutlinedWrapped` draws
// underneath every glyph. Against black, the dark-theme variant is the legible
// one whatever the desktop is set to.
const SDL_Color kCaution  = {0xFC, 0xE1, 0x00, 0xFF};   // Theme fillCaution  #FCE100
const SDL_Color kCritical = {0xFF, 0x99, 0xA4, 0xFF};   // Theme fillCritical #FF99A4

// How long a mark holds before it may clear, when the person has asked for
// reduced motion. `windows_v2.md` D3 fades the mark in and out over 167 ms;
// an SDL overlay has no fade, it has a hard bitmap swap, so a reading that
// oscillates across a threshold is a strobe rather than a shimmer. Microsoft's
// own description of the setting names "flashing, blinking, flickering" first,
// so with motion off the mark latches: it still changes when the *state*
// changes, it just refuses to blink. Three seconds is the shortest hold that
// covers an oscillation at the 1 Hz sample rate.
const uint32_t kReducedMotionHoldMs = 3000;

// ---------------------------------------------------------------------------
// State

struct State {
    bool active = false;
    bool warningsEnabled = true;
    QString machineName;
    int negotiatedFps = 60;
    int bitrateKbps = 0;
    double frameIntervalMs = 1000.0 / 60.0;
    Thresholds th;

    uint32_t beganAtMs = 0;
    uint32_t lastTickMs = 0;

    // Written by the decoder thread in onStats(), read by the main thread in
    // tick(). The spin lock is SDL's, and this file is not the first to use it
    // that way: d3d11va.cpp guards its overlay textures with the same type.
    SDL_SpinLock statsLock = 0;
    VIDEO_STATS stats = {};
    bool haveStats = false;
    uint32_t lastFrameAtMs = 0;

    // The host's own one-bit verdict, from the control thread.
    SDL_atomic_t hostPoor = {};

    // What is currently on the status overlay, so it is redrawn only when it
    // changes — D3's "only when the reading changes", and also the difference
    // between one rasterise a second and one per state change.
    char drawn[256] = {};
    Verdict drawnVerdict = VerdictUnknown;
    uint32_t drawnAtMs = 0;

    // Set once frames have stopped for longer than gapSeconds, so end() can
    // say what happened rather than leaving upstream to print a number.
    bool sawGap = false;
    double longestGapSeconds = 0.0;
};

State g;

// ---------------------------------------------------------------------------
// One dimension's reading

struct Reading {
    bool known = false;
    Verdict verdict = VerdictUnknown;
    double value = 0.0;

    // How far past the caution bar, as a multiple of it. Only used to break a
    // tie between two dimensions at the same verdict, and it matters because
    // the cause word is the whole of what the mark says: with 17% loss and
    // 95 ms of round trip both critical, naming the one that happens to come
    // first in the enum tells a person the wrong thing to go and look at.
    double severity = 0.0;
};

Reading grade(double value, double caution, double critical, bool known)
{
    Reading r;
    r.known = known;
    r.value = value;
    if (!known) {
        r.verdict = VerdictUnknown;
    }
    else if (value >= critical) {
        r.verdict = VerdictCritical;
    }
    else if (value >= caution) {
        r.verdict = VerdictCaution;
    }
    else {
        r.verdict = VerdictGood;
    }
    if (known && caution > 0.0) {
        r.severity = value / caution;
    }
    return r;
}

// Frame rate is the one dimension where *lower* is worse, so it grades the
// shortfall rather than the value.
Reading gradeFps(double fps, double negotiated, const Thresholds& th, bool known)
{
    Reading r;
    r.known = known && negotiated > 0;
    r.value = fps;
    if (!r.known) {
        r.verdict = VerdictUnknown;
    }
    else {
        const double ratio = fps / negotiated;
        r.verdict = ratio < th.fpsCritical ? VerdictCritical
                  : ratio < th.fpsCaution  ? VerdictCaution
                                           : VerdictGood;
        // Inverted, because this is the one dimension where lower is worse,
        // so the severity has to be too: at half the frame rate asked for,
        // fpsCaution / ratio is roughly 1.8.
        r.severity = ratio > 0.0 ? th.fpsCaution / ratio : 0.0;
    }
    return r;
}

const char* causeWord(Dimension d)
{
    // Short phrases, and upstream's own vocabulary where it has one.
    // `windows_v2.md` D3 gives "packet loss" and "host latency" as the
    // examples, so two words is the shape rather than one token.
    switch (d) {
    case DimFrameRate:      return "frame rate";
    case DimHostLatency:    return "host latency";
    case DimNetworkLatency: return "network latency";
    case DimPacketLoss:     return "packet loss";
    case DimFramePacing:    return "frame pacing";
    default:                return "stream";
    }
}

const char* verdictWord(Verdict v)
{
    switch (v) {
    case VerdictGood:     return "good";
    case VerdictCaution:  return "caution";
    case VerdictCritical: return "critical";
    case VerdictWaiting:  return "waiting";
    default:              return "unknown";
    }
}

// The five readings, from the two-window statistics upstream already computes.
struct Sample {
    Reading dim[DimCount];
    Verdict verdict = VerdictUnknown;
    Dimension worst = DimFrameRate;
    bool anyKnown = false;

    // Carried through to the panel and the log line.
    double decodeMs = 0.0, queueMs = 0.0, renderMs = 0.0;
    double decodedFps = 0.0, renderedFps = 0.0;
    double hostMinMs = 0.0, hostMaxMs = 0.0;
    uint32_t rttMs = 0, rttVarianceMs = 0;
};

Sample evaluate(const VIDEO_STATS& s, const Thresholds& th, int negotiatedFps, double frameIntervalMs)
{
    Sample out;

    // ---- frames arriving --------------------------------------------------
    // `receivedFps` is the rate frames arrived from the network over the two
    // one-second windows, which is the number `gaming-rig-e2e` means by
    // "frames per second at the client".
    out.dim[DimFrameRate] = gradeFps(s.receivedFps, negotiatedFps, th, s.receivedFps > 0);
    out.decodedFps = s.decodedFps;
    out.renderedFps = s.renderedFps;

    // ---- host processing latency -----------------------------------------
    // Unknown when the host does not report it: the field is documented as
    // zero in that case (Limelight.h:150-155), so the counter never advances
    // and the average would be a division by zero.
    const bool hostKnown = s.framesWithHostProcessingLatency > 0;
    const double hostMs = hostKnown
        ? (double)s.totalHostProcessingLatency / 10.0 / s.framesWithHostProcessingLatency
        : 0.0;
    out.hostMinMs = s.minHostProcessingLatency / 10.0;
    out.hostMaxMs = s.maxHostProcessingLatency / 10.0;
    out.dim[DimHostLatency] = grade(hostMs,
                                    th.hostCaution * frameIntervalMs,
                                    th.hostCritical * frameIntervalMs,
                                    hostKnown);

    // ---- round trip -------------------------------------------------------
    // Unknown when ENet could not estimate it. Upstream prints "N/A" for
    // exactly this and we must not print a zero, which would read as perfect.
    out.rttMs = s.lastRtt;
    out.rttVarianceMs = s.lastRttVariance;
    out.dim[DimNetworkLatency] = grade(s.lastRtt, th.rttCautionMs, th.rttCriticalMs, s.lastRtt != 0);

    // ---- loss and pacing --------------------------------------------------
    const bool lossKnown = s.totalFrames > 0;
    out.dim[DimPacketLoss] = grade(lossKnown ? (double)s.networkDroppedFrames / s.totalFrames * 100.0 : 0.0,
                                   th.lossCautionPct, th.lossCriticalPct, lossKnown);

    const bool pacingKnown = s.decodedFrames > 0;
    out.dim[DimFramePacing] = grade(pacingKnown ? (double)s.pacerDroppedFrames / s.decodedFrames * 100.0 : 0.0,
                                    th.pacingCautionPct, th.pacingCriticalPct, pacingKnown);

    if (s.decodedFrames > 0) {
        out.decodeMs = (double)s.totalDecodeTimeUs / 1000.0 / s.decodedFrames;
    }
    if (s.renderedFrames > 0) {
        out.queueMs = (double)s.totalPacerTimeUs / 1000.0 / s.renderedFrames;
        out.renderMs = (double)s.totalRenderTimeUs / 1000.0 / s.renderedFrames;
    }

    // ---- the verdict is the worst of the ones that could be graded --------
    for (int i = 0; i < DimCount; i++) {
        if (!out.dim[i].known) {
            continue;
        }
        out.anyKnown = true;
        if (out.dim[i].verdict > out.verdict
                || (out.dim[i].verdict == out.verdict
                    && out.dim[i].severity > out.dim[out.worst].severity)) {
            out.verdict = out.dim[i].verdict;
            out.worst = (Dimension)i;
        }
    }
    if (!out.anyKnown) {
        out.verdict = VerdictUnknown;
    }

    return out;
}

// ---------------------------------------------------------------------------
// The mark
//
// Four bars, in ASCII, because ModeSeven.ttf has no block-drawing characters
// (verified against app/ModeSeven.ttf: 101 glyphs, printable ASCII less `~`,
// plus U+00A3, U+00BC-BE, U+00F7 and U+2010). A bar drawn in U+2588 would be
// a bar nobody sees.
//
// The deduction is per graded dimension, one for a caution and two for a
// critical, floored at one bar so the mark never becomes an empty bracket
// that reads as "no signal at all".
void barsFor(const Sample& s, Verdict v, char* out, size_t len)
{
    int bars = 4;
    for (int i = 0; i < DimCount; i++) {
        if (!s.dim[i].known) {
            continue;
        }
        bars -= s.dim[i].verdict == VerdictCritical ? 2
              : s.dim[i].verdict == VerdictCaution  ? 1 : 0;
    }

    // The verdict can be raised above our own readings, by the host's own
    // signal, which has no dimension here to deduct from. Four full bars over
    // a red mark would be the mark contradicting itself.
    if (v == VerdictCritical && bars > 2) bars = 2;
    if (v == VerdictCaution && bars > 3) bars = 3;

    if (bars < 1) bars = 1;
    if (bars > 4) bars = 4;

    char buf[7] = "[....]";
    for (int i = 0; i < bars; i++) {
        buf[1 + i] = '|';
    }
    SDL_strlcpy(out, buf, len);
}

// ---------------------------------------------------------------------------
// The panel
//
// The same ten facts `stringifyVideoStats` prints, grouped and labelled
// Video / Network / Host, per `windows_v2.md` D3. Two of them can read
// `unknown`, and where they do they say so rather than showing a zero.
//
// What is deliberately not here is upstream's header line — the resolution and
// codec. It is computed inside `stringifyVideoStats` from decoder state this
// file cannot see without refactoring an upstream function, and it is already
// written to the log once per stream as "Video stream is %dx%dx%d", which is
// the line `gaming-rig-e2e` step 7 greps for.
void writePanel(const Sample& s, char* out, int len)
{
    char bars[8];
    barsFor(s, s.verdict, bars, sizeof(bars));

    char host[64], rtt[64];
    if (s.dim[DimHostLatency].known) {
        snprintf(host, sizeof(host), "%.1f ms  (min %.1f  max %.1f)",
                 s.dim[DimHostLatency].value, s.hostMinMs, s.hostMaxMs);
    }
    else {
        // Never a zero, and never blank. "unknown" is its own answer.
        SDL_strlcpy(host, "unknown  (this host does not report it)", sizeof(host));
    }

    if (s.dim[DimNetworkLatency].known) {
        snprintf(rtt, sizeof(rtt), "%u ms  (variance %u ms)", s.rttMs, s.rttVarianceMs);
    }
    else {
        SDL_strlcpy(rtt, "unknown", sizeof(rtt));
    }

    snprintf(out, len,
             "Omnuv  %s  %s%s%s\n"
             "\n"
             "VIDEO\n"
             "  frames in       %.1f / %d fps\n"
             "  decoded         %.1f fps\n"
             "  rendered        %.1f fps\n"
             "  decode          %.2f ms\n"
             "  queue           %.2f ms\n"
             "  render          %.2f ms\n"
             "  dropped pacing  %.2f %%\n"
             "\n"
             "NETWORK\n"
             "  round trip      %s\n"
             "  packet loss     %.2f %%\n"
             "\n"
             "HOST\n"
             "  produce frame   %s\n"
             "\n"
             "Ctrl+Alt+Shift+S hides this. The same numbers go to\n"
             "the log once a second, which is what to send for support.\n",
             bars,
             verdictWord(s.verdict),
             s.verdict == VerdictCaution || s.verdict == VerdictCritical ? "  " : "",
             s.verdict == VerdictCaution || s.verdict == VerdictCritical ? causeWord(s.worst) : "",
             s.dim[DimFrameRate].value, g.negotiatedFps,
             s.decodedFps,
             s.renderedFps,
             s.decodeMs,
             s.queueMs,
             s.renderMs,
             s.dim[DimFramePacing].known ? s.dim[DimFramePacing].value : 0.0,
             rtt,
             s.dim[DimPacketLoss].known ? s.dim[DimPacketLoss].value : 0.0,
             host);
}

// ---------------------------------------------------------------------------
// The log line
//
// The only half of this a harness can read. `scripts/gaming-rig-e2e` samples
// at 10, 30 and 60 seconds over `qm guest exec`, which can read a file and
// cannot see a pixel, so this is what step 7's four thresholds are asserted
// against. One line per second, whether or not the panel is on and whether or
// not frames are arriving — a gap shows up as `fps=0.00`, which is the reading
// that matters most and the one a frame-driven emitter cannot produce.
//
// `SDL_LogInfo` on SDL_LOG_CATEGORY_APPLICATION reaches
// %TEMP%\Moonlight-<epoch>.log through `sdlLogToDiskHandler` in app/main.cpp;
// only the `list` action suppresses INFO, and `stream` is not `list`.
// Never stdout: that channel belongs to `signin`, and the change-budget
// workflow fails the build if this file writes to it.
void writeLogLine(const Sample& s, uint32_t elapsedS, double gapS, Verdict v)
{
    char fps[32], host[32], rtt[32];

    if (s.dim[DimFrameRate].known) {
        snprintf(fps, sizeof(fps), "%.2f/%d", s.dim[DimFrameRate].value, g.negotiatedFps);
    }
    else {
        snprintf(fps, sizeof(fps), "0.00/%d", g.negotiatedFps);
    }

    // `na` and not `0`, for the same reason the panel says "unknown": a zero
    // here would be read by the harness as a perfect score.
    if (s.dim[DimHostLatency].known) {
        snprintf(host, sizeof(host), "%.2f", s.dim[DimHostLatency].value);
    }
    else {
        SDL_strlcpy(host, "na", sizeof(host));
    }

    if (s.dim[DimNetworkLatency].known) {
        snprintf(rtt, sizeof(rtt), "%u", s.rttMs);
    }
    else {
        SDL_strlcpy(rtt, "na", sizeof(rtt));
    }

    // Built with the C library's snprintf and handed to SDL as one %s, which
    // is what upstream does with every float it logs (`logVideoStats` prints
    // `stringifyVideoStats`'s buffer the same way). SDL ships its own
    // vsnprintf rather than using the platform's, and nothing else in this
    // tree puts a %f through SDL_Log -- a format SDL renders differently from
    // the C library would be a silent change to a channel a harness parses.
    char line[512];
    snprintf(line, sizeof(line),
             "Omnuv stream quality: t=%u fps=%s host=%s rtt=%s loss=%.2f jitter=%.2f "
             "decode=%.2f queue=%.2f render=%.2f gap=%.1f verdict=%s cause=%s",
             elapsedS,
             fps, host, rtt,
             s.dim[DimPacketLoss].known ? s.dim[DimPacketLoss].value : 0.0,
             s.dim[DimFramePacing].known ? s.dim[DimFramePacing].value : 0.0,
             s.decodeMs, s.queueMs, s.renderMs,
             gapS,
             verdictWord(v),
             (v == VerdictCaution || v == VerdictCritical) ? causeWord(s.worst) : "-");

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", line);
}

// The machine's name, trimmed so the mark's first line stays inside the wrap
// width. `RenderTextOutlinedWrapped` drops the black outline on any line it
// has to wrap, and an unoutlined line over bright video is unreadable.
QString shortName()
{
    const QString& n = g.machineName;
    return n.length() <= 24 ? n : n.left(23) + ".";
}

void draw(const char* text, const SDL_Color& colour, Verdict v, uint32_t nowMs)
{
    Overlay::OverlayManager& om = Session::get()->getOverlayManager();

    if (text == nullptr || text[0] == '\0') {
        if (g.drawn[0] != '\0') {
            om.setOverlayState(Overlay::OverlayStatusUpdate, false);
            g.drawn[0] = '\0';
            g.drawnVerdict = VerdictUnknown;
        }
        return;
    }

    if (SDL_strcmp(g.drawn, text) == 0) {
        return;
    }

    SDL_strlcpy(g.drawn, text, sizeof(g.drawn));
    g.drawnVerdict = v;
    g.drawnAtMs = nowMs;
    om.setOverlayColor(Overlay::OverlayStatusUpdate, colour);
    om.updateOverlayText(Overlay::OverlayStatusUpdate, text);
    om.setOverlayState(Overlay::OverlayStatusUpdate, true);
}

}  // namespace

namespace OmnuvStreamQuality {

Thresholds thresholdsFromEnvironment(const char* value)
{
    Thresholds th;
    if (value == nullptr || value[0] == '\0') {
        return th;
    }

    struct { const char* name; double* slot; } table[] = {
        { "fps_caution",      &th.fpsCaution },
        { "fps_critical",     &th.fpsCritical },
        { "host_caution",     &th.hostCaution },
        { "host_critical",    &th.hostCritical },
        { "rtt_caution",      &th.rttCautionMs },
        { "rtt_critical",     &th.rttCriticalMs },
        { "loss_caution",     &th.lossCautionPct },
        { "loss_critical",    &th.lossCriticalPct },
        { "pacing_caution",   &th.pacingCautionPct },
        { "pacing_critical",  &th.pacingCriticalPct },
        { "gap_seconds",      &th.gapSeconds },
    };

    const QList<QByteArray> pairs = QByteArray(value).split(',');
    for (const QByteArray& pair : pairs) {
        const QByteArray trimmed = pair.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const int eq = trimmed.indexOf('=');
        if (eq <= 0) {
            qWarning("Omnuv: OMNUV_STREAM_QUALITY ignoring %s (not name=value)", trimmed.constData());
            continue;
        }
        const QByteArray name = trimmed.left(eq).trimmed();
        bool ok = false;
        const double v = trimmed.mid(eq + 1).trimmed().toDouble(&ok);

        // A threshold that failed to parse keeps its default rather than
        // becoming zero. Zero grades everything critical, which is the loudest
        // possible way for a typo to be wrong.
        if (!ok || !std::isfinite(v) || v < 0.0) {
            qWarning("Omnuv: OMNUV_STREAM_QUALITY ignoring %s (not a number)", trimmed.constData());
            continue;
        }

        bool found = false;
        for (auto& entry : table) {
            if (name == entry.name) {
                *entry.slot = v;
                found = true;
                break;
            }
        }
        if (!found) {
            qWarning("Omnuv: OMNUV_STREAM_QUALITY ignoring unknown threshold %s", name.constData());
        }
    }

    return th;
}

void begin(const QString& machineName, int negotiatedFps, int bitrateKbps, bool warningsEnabled)
{
    g = State();
    g.active = true;
    g.warningsEnabled = warningsEnabled;
    g.machineName = machineName;
    g.negotiatedFps = negotiatedFps > 0 ? negotiatedFps : 60;
    g.bitrateKbps = bitrateKbps;
    g.frameIntervalMs = 1000.0 / g.negotiatedFps;
    g.th = thresholdsFromEnvironment(SDL_getenv("OMNUV_STREAM_QUALITY"));
    g.beganAtMs = g.lastTickMs = g.lastFrameAtMs = SDL_GetTicks();
    SDL_AtomicSet(&g.hostPoor, 0);

    // The thresholds in the log, once, because a reading is only judgeable
    // against the bar it was judged by — and because the knob above means the
    // bar is not necessarily the one in the source.
    char line[512];
    snprintf(line, sizeof(line),
             "Omnuv stream quality thresholds: fps=%.2f/%.2f x %d host=%.2f/%.2f x %.2fms "
             "rtt=%.0f/%.0fms loss=%.2f/%.2f%% pacing=%.2f/%.2f%% gap=%.1fs warnings=%s",
             g.th.fpsCaution, g.th.fpsCritical, g.negotiatedFps,
             g.th.hostCaution, g.th.hostCritical, g.frameIntervalMs,
             g.th.rttCautionMs, g.th.rttCriticalMs,
             g.th.lossCautionPct, g.th.lossCriticalPct,
             g.th.pacingCautionPct, g.th.pacingCriticalPct,
             g.th.gapSeconds,
             warningsEnabled ? "on" : "off");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", line);
}

void onStats(const VIDEO_STATS& stats)
{
    if (!g.active) {
        return;
    }
    const uint32_t now = SDL_GetTicks();
    SDL_AtomicLock(&g.statsLock);
    g.stats = stats;
    g.haveStats = true;
    g.lastFrameAtMs = now;
    SDL_AtomicUnlock(&g.statsLock);
}

void onHostVerdict(int connectionStatus)
{
    // CONN_STATUS_POOR is the only reading here that is not ours: the host
    // measures frame loss over three-second intervals and says so
    // (ControlStream.c:488-503). It is a floor on the verdict rather than the
    // verdict, because it is one bit and it is blind to latency entirely --
    // and because `connectionSawFrame` only runs when a frame arrives, so a
    // cable that has been pulled produces silence from it, not a warning.
    SDL_AtomicSet(&g.hostPoor, connectionStatus == CONN_STATUS_POOR ? 1 : 0);
}

void tick(bool statusSlotBusy)
{
    if (!g.active) {
        return;
    }

    const uint32_t now = SDL_GetTicks();
    if (now - g.lastTickMs < 1000) {
        return;
    }
    g.lastTickMs = now;

    VIDEO_STATS stats;
    bool haveStats;
    uint32_t lastFrameAtMs;
    SDL_AtomicLock(&g.statsLock);
    stats = g.stats;
    haveStats = g.haveStats;
    lastFrameAtMs = g.lastFrameAtMs;
    SDL_AtomicUnlock(&g.statsLock);

    const double gapS = (double)(now - lastFrameAtMs) / 1000.0;
    const uint32_t elapsedS = (now - g.beganAtMs) / 1000;

    Sample s;
    if (haveStats) {
        s = evaluate(stats, g.th, g.negotiatedFps, g.frameIntervalMs);
    }

    // Let the stream settle before grading it, for the same span and the same
    // reason moonlight-common-c does: `connectionSawFrame` suppresses its own
    // warnings for CONN_STATUS_SAMPLE_PERIOD, 3000 ms
    // (ControlStream.c:128, 476-485), "to allow the network and host to
    // settle". A mark drawn over the first frames of every stream is a mark
    // that means "a stream is starting".
    const bool settled = (now - g.beganAtMs) >= 3000;

    // **Frames have stopped, and something is still driving it.**
    //
    // This is a state and not a failure, and the distinction is the same one
    // CLAUDE.md's Periodic Progress Check draws between *working* and
    // *unattended*: ENet is retransmitting with backoff for ten seconds
    // (enet_peer_timeout(peer, 2, 10000, 10000)) before it gives up, so for
    // those ten seconds the stream is genuinely being re-established. Drawing
    // that as an error would be the defect the console removed from its
    // machine ladder.
    //
    // Nothing here *performs* a reconnect. The transport already does, and
    // adding a second one above it would be a state machine racing the one
    // that works. What was missing was anybody saying so.
    const bool waiting = gapS >= g.th.gapSeconds;
    if (waiting) {
        g.sawGap = true;
        if (gapS > g.longestGapSeconds) {
            g.longestGapSeconds = gapS;
        }
    }

    Verdict v;
    if (waiting) {
        // Not gated on `settled`: a stream that never produced a frame at all
        // is precisely the case worth naming, and it is what a pulled cable
        // during startup looks like.
        v = VerdictWaiting;
    }
    else if (!settled) {
        // Not graded yet, so not good either. `unknown` is already this
        // design's word for "no verdict", and saying `good` here would put a
        // pass in the harness's channel for a window in which nothing was
        // judged -- which is the exact shape of a check that cannot fail. The
        // numbers are on the same line regardless.
        v = VerdictUnknown;
    }
    else if (SDL_AtomicGet(&g.hostPoor) && s.verdict < VerdictCritical) {
        // The host says the connection is poor and our own numbers do not
        // agree. Take the host's word: it can see loss on the path we cannot.
        v = VerdictCritical;
        s.worst = DimPacketLoss;
    }
    else {
        v = s.verdict;
    }

    writeLogLine(s, elapsedS, gapS, v);

    // The panel is opt-in and is numbers on purpose -- the rule against
    // putting a number in front of a buyer is about codes they cannot act on,
    // not about a diagnostic they asked for with a hotkey.
    Overlay::OverlayManager& om = Session::get()->getOverlayManager();
    if (haveStats && om.isOverlayEnabled(Overlay::OverlayDebug)) {
        char panel[1024];
        writePanel(s, panel, sizeof(panel));
        om.updateOverlayText(Overlay::OverlayDebug, panel);
    }

    // Upstream's mouse-emulation indicator owns the same slot while it is up,
    // and upstream's own `connectionWarnings` preference decides whether this
    // channel may draw at all. The log line above is written either way: a log
    // is not a banner, and it is the only thing the harness can read.
    if (statusSlotBusy || !g.warningsEnabled) {
        // Forget what we believe is on the slot. Upstream's mouse-emulation
        // indicator writes the same overlay and turns it *off* when it ends
        // (session.cpp:1553-1557), so a mark we still think is up would never
        // be redrawn -- draw() suppresses an unchanged string, which is the
        // right thing to do right up until somebody else cleared the surface
        // underneath it.
        g.drawn[0] = '\0';
        return;
    }

    char text[256];

    if (v == VerdictWaiting) {
        // The number keeps counting under reduced motion, and that is the
        // same exception Theme.qml already makes for the tray's connecting
        // ring: a changing number here is not an escort to the message, it
        // *is* the message -- it is the only evidence that something is still
        // being attempted. A frozen "No picture for 2s" would read as give up.
        // Three short lines, each inside the wrap width, so the outline
        // survives. The stop is the combination upstream already binds
        // (KeyComboQuit, Ctrl+Alt+Shift+Q in app/streaming/input/input.cpp:83)
        // rather than a button, because there is no cursor over a captured
        // stream and no Qt to draw one with.
        snprintf(text, sizeof(text),
                 "Reconnecting to %s\nNo picture for %.0fs\nCtrl+Alt+Shift+Q to stop waiting",
                 shortName().toUtf8().constData(), gapS);
        draw(text, kCaution, v, now);
        return;
    }

    if (v == VerdictGood || v == VerdictUnknown) {
        // Green never draws, and neither does "I could not tell". A mark that
        // means "no reading" trains everyone to ignore marks.
        //
        // Under reduced motion a mark that is already up holds briefly before
        // it may clear, so a reading oscillating across a threshold shows one
        // steady mark instead of a strobe.
        if (g.drawn[0] != '\0'
                && !OmnuvAppearance::animationsEnabledNow()
                && now - g.drawnAtMs < kReducedMotionHoldMs) {
            return;
        }
        draw(nullptr, kCaution, v, now);
        return;
    }

    char bars[8];
    barsFor(s, v, bars, sizeof(bars));

    // Upstream's own advice, on upstream's own condition: its banner read
    // "Slow connection to PC / Reduce your bitrate" above 5 Mbps and "Poor
    // connection to PC" below. Kept, narrowed to the cause it actually
    // applies to, because a buyer losing frames at 40 Mbps has something they
    // can do about it and a buyer with 60 ms of latency does not.
    const bool advise = s.worst == DimPacketLoss && g.bitrateKbps > 5000;
    snprintf(text, sizeof(text), "%s %s%s", bars, causeWord(s.worst),
             advise ? "\nTry a lower bitrate" : "");
    draw(text, v == VerdictCritical ? kCritical : kCaution, v, now);
}

QString lostPictureSentence()
{
    if (!g.sawGap) {
        return QString();
    }

    // Upstream's own message here is "Connection terminated" with an error
    // code under it, which tells a buyer nothing they can act on. We watched
    // the picture stop and we know how long ago, so say that instead. No
    // number they cannot use, and no invented cause — what broke between here
    // and the machine is not something this end can know.
    return QObject::tr("The picture from %1 stopped arriving %2 seconds ago and did not come back.")
            .arg(g.machineName)
            .arg(qRound(g.longestGapSeconds));
}

void end()
{
    if (!g.active) {
        return;
    }

    char line[128];
    snprintf(line, sizeof(line), "Omnuv stream quality: ended after %us, longest gap %.1fs",
             (SDL_GetTicks() - g.beganAtMs) / 1000, g.longestGapSeconds);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s", line);

    g.active = false;
    g.drawn[0] = '\0';

    // Take the mark down. It described a stream that no longer exists, and
    // leaving the slot on means the next one starts under the last one's
    // verdict -- begin() resets our own record of what is drawn but cannot
    // know what somebody else left on the overlay.
    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayStatusUpdate, false);
}

}
