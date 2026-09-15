#pragma once

#include <QString>

#include "SDL_compat.h"
#include "streaming/video/decoder.h"

// **What the stream is doing, said in words a person can act on, while it is
// happening.**
//
// Upstream computes ten statistics and presents them as ten lines of yellow
// text on a hotkey. That is a diagnostic, and it is a good one. It is not an
// answer to the question a person actually has mid-game, which is *is this me,
// is it the network, or is it the machine* — and it is no answer at all to
// *why did the picture stop*, because upstream draws nothing during the ten
// seconds the transport spends trying to get it back.
//
// This object turns those numbers into three things:
//
//   a mark       drawn only when the reading is bad, with the cause in words
//   a panel      the same numbers grouped Video / Network / Host
//   a state      "Reconnecting to <machine>" while frames are not arriving
//
// and one line per second in the log, which is the only one of the four a
// harness can read. `scripts/gaming-rig-e2e` step 7 samples at 10, 30 and 60
// seconds; nothing drawn over video is reachable from `qm guest exec`, so the
// log line is not a convenience, it is the entire machine-readable half.
//
// ---------------------------------------------------------------------------
// Where it draws, and why that decided the design
//
// Qt does not run during a stream. `app/streaming/session.cpp` hijacks its
// thread to be SDL's main thread precisely so that it does not — the comment
// there says "we want to suspend all Qt processing until the stream is over" —
// and `app/gui/StreamSegue.qml` runs `gc()` before starting for the same
// reason. So the QML overlay `windows_impl.md` I4 asks for cannot exist. What
// exists is `Overlay::OverlayManager`, which rasterises a string into an
// `SDL_Surface` that every video renderer already composites.
//
// That is what we write to, and it decides three things the design did not
// anticipate:
//
//   position   is hard-coded per overlay type in all six renderers
//              (`d3d11va.cpp:847`, `sdlvid.cpp:225`, and four more). A glyph
//              in the top-right corner costs six upstream files; the status
//              slot that already exists is bottom-left, and that is where the
//              mark goes.
//   typeface   is ModeSeven.ttf, which maps 101 glyphs: printable ASCII less
//              `~`, plus a pound sign, three vulgar fractions, a division
//              sign and U+2010. Verified with fontTools against
//              `app/ModeSeven.ttf`. Every block-drawing character is absent,
//              so the four bars are `[||||]` in ASCII and not U+2588. A bar
//              drawn in a character the font does not have is a bar nobody
//              sees.
//   colour     is a member of the overlay with a getter and no setter, so
//              amber and red needed one added. That is the whole of the
//              change to `overlaymanager.(h|cpp)`.
//
// ---------------------------------------------------------------------------
// One reading is not a verdict
//
// Every dimension answers `pass`, `fail` or **`unknown`**, and `unknown` is a
// first-class answer rather than a zero. `docs/client-widget.md` states the
// rule and `omnuv_protocol::CheckResult` already carries it across the wire:
// *never to be shown as a pass, and never as a failure either*.
//
// It is reachable here, on two of the five dimensions, and on real hosts:
//
//   host latency    `du->frameHostProcessingLatency` is zero when the host
//                   does not report it (`Limelight.h:150-155`), so
//                   `framesWithHostProcessingLatency` stays at zero and the
//                   average is a division by nothing.
//   network latency `LiGetEstimatedRttInfo()` fails on very old GFE and on a
//                   peer that has gone away, and upstream prints the literal
//                   string "N/A" for it (`ffmpeg.cpp:940-948`).
//
// An unknown dimension abstains from the verdict. It never counts as good,
// and it never draws a mark — a mark that means "I could not look" trains
// everyone to ignore marks.
namespace OmnuvStreamQuality {

// The five dimensions, and the worst of them is the verdict.
enum Dimension {
    DimFrameRate,       // are frames arriving at the rate we negotiated
    DimHostLatency,     // how long the machine takes to produce one
    DimNetworkLatency,  // round trip on the control stream
    DimPacketLoss,      // frames the network lost
    DimFramePacing,     // frames this client dropped to keep the pace
    DimCount
};

enum Verdict {
    VerdictUnknown,   // nothing could be graded
    VerdictGood,      // draws nothing; green is the absence of a mark
    VerdictCaution,
    VerdictCritical,
    VerdictWaiting    // frames stopped arriving and the transport is retrying
};

// **The thresholds, each with the reason it is that number, and a knob.**
//
// `OMNUV_STREAM_QUALITY` overrides any of them as a comma-separated list of
// `name=value` — `OMNUV_STREAM_QUALITY=rtt_caution=120,loss_caution=3.5`. Two
// reasons it exists rather than being a constant somebody recompiles:
// `windows_v2.md` D3 says the thresholds are "tuned on hardware, because every
// network reads differently and the calibration knob has to exist", and the
// lab rig is driven by `qm guest exec`, where an environment variable is the
// only thing a harness can set.
//
// Anything not named keeps its default. An unparseable value is ignored and
// logged, rather than silently becoming zero — a threshold of zero grades
// everything critical, which is the loudest possible way to be wrong.
struct Thresholds {
    // ---- frames arriving, as a fraction of what was negotiated -----------
    //
    // `scripts/gaming-rig-e2e` asks for ">= 55 of 60 over the last 30 s". As
    // an absolute that number is dishonest twice over: at 30 fps it is
    // unreachable and the mark would be permanently on, and at 120 Hz it is
    // met while half the frames are missing. 55/60 is 0.9167, so the ratio is
    // what the lab actually meant and the ratio is what generalises.
    double fpsCaution = 0.92;
    // Below three quarters the motion is not degraded, it is broken.
    double fpsCritical = 0.75;

    // ---- host processing latency, as a fraction of one frame interval ----
    //
    // Also relative, and for the same reason: 12 ms is 72% of a 60 Hz frame,
    // 36% of a 30 Hz one and *more than a whole frame* at 120. A host that
    // needs longer than one interval to produce a frame cannot sustain the
    // rate it agreed to, so the interval is the natural unit and one interval
    // is the natural red.
    //
    // At 60 fps these are 12.5 ms and 16.7 ms, which is where the lab's 12 ms
    // came from.
    double hostCaution = 0.75;
    double hostCritical = 1.00;

    // ---- round trip, in milliseconds -------------------------------------
    //
    // **Deliberately not the lab's 8 ms.** That is a same-building number and
    // it is the right bar for `gaming-rig-e2e` to assert; it is the wrong bar
    // to grade a buyer against, because a buyer streaming from a machine in
    // the next city has 25 ms and a stream they are happy with. A mark that
    // is on for every real customer is a mark nobody reads.
    //
    // 40 ms is roughly where added round trip starts being felt in a mouse
    // rather than measured in a tool; 80 ms is where aiming stops working.
    // These two are judgement and they are the two most likely to be wrong,
    // which is what the knob above is for.
    double rttCautionMs = 40.0;
    double rttCriticalMs = 80.0;

    // ---- frames the network lost, as a percentage ------------------------
    //
    // The red is not ours: `CONN_OKAY_LOSS_RATE` is 5 in
    // `moonlight-common-c/src/ControlStream.c:127`, and it is the rate below
    // which upstream itself is willing to call a connection okay. Above it,
    // upstream's own host-side heuristic has stopped saying okay, so it is the
    // honest place for us to stop saying okay too.
    //
    // The amber is ours, and it sits between the lab's pass bar and upstream's
    // okay bar on purpose: at 1080p60, 2% is a dropped frame every 0.8 s,
    // which is visible as a hitch. `windows_v2.md` D3 wants the mark "before
    // the stream degrades visibly", and 5% is well after. The lab's own 1%
    // stays tighter than this, so a clean lab run never draws a mark.
    double lossCautionPct = 2.0;
    double lossCriticalPct = 5.0;

    // ---- frames this client dropped to keep pace, as a percentage --------
    //
    // A different fact with the same unit, which is why `gaming-rig-e2e`'s
    // single "frames dropped < 1%" is ambiguous and should be read as the
    // network one. This is the Pacer discarding a frame because it arrived
    // for a vsync that had already gone — jitter, or a display that cannot
    // keep up. Same numbers, different word, so the cause names which.
    double pacingCautionPct = 2.0;
    double pacingCriticalPct = 5.0;

    // ---- how long with no frame at all before it is a state --------------
    //
    // Not a quality reading: the picture has stopped. Two seconds because the
    // tick runs at 1 Hz, so anything shorter cannot be told apart from the
    // tick's own jitter, and because the transport's own deadline is ten
    // (`enet_peer_timeout(peer, 2, 10000, 10000)`,
    // `moonlight-common-c/src/ControlStream.c:1837`). Two leaves eight
    // seconds of visible, honest waiting before the session actually ends.
    double gapSeconds = 2.0;
};

// Read once at stream start. Exposed for the test in streamquality.cpp.
Thresholds thresholdsFromEnvironment(const char* value);

// Stream start. `machineName` is what a person calls it; `negotiatedFps` is
// what the two ends agreed, which is what every frame-rate reading is
// measured against. `warningsEnabled` is upstream's `connectionWarnings`
// preference: when a person has turned the banner off, the mark and the
// waiting state go with it. The log line does not — a log is not a banner,
// and it is the only thing the harness can read.
// `bitrateKbps` is carried only so the mark can keep the one piece of advice
// upstream's banner gave: above 5 Mbps its "Poor connection to PC" became
// "Slow connection to PC / Reduce your bitrate". That is actionable and a
// buyer had it, so it survives the redesign rather than being traded for a
// tidier mark.
void begin(const QString& machineName, int negotiatedFps, int bitrateKbps, bool warningsEnabled);

// Once a second, from the decoder thread, with the two-window statistics
// upstream already computes for the overlay. Stores them; draws nothing.
void onStats(const VIDEO_STATS& stats);

// The host's own verdict, from `clConnectionStatusUpdate`. It is the only
// signal here that is not ours, and it is one bit wide: it fires on frame
// loss measured at the host over three-second intervals
// (`ControlStream.c:488-503`). Treated as a floor, never as the whole answer.
void onHostVerdict(int connectionStatus);

// Every pass of the SDL event loop, on the main thread. Rate-limits itself to
// 1 Hz, so calling it at input rate costs one clock read. `statusSlotBusy` is
// upstream's mouse-emulation indicator holding the same overlay slot.
void tick(bool statusSlotBusy);

// A sentence for the failure panel, when the stream ended while frames were
// already missing — the case where upstream's own message is "Connection
// terminated" with an error code under it and a buyer learns nothing. Empty
// otherwise, and the caller keeps its own wording.
//
// Read-only, because it is called from moonlight-common-c's control thread in
// `clConnectionTerminated` while the main thread may be inside tick().
QString lostPictureSentence();

// Stream end, on the main thread: stop drawing and write the summary. Safe to
// call twice.
void end();

}
