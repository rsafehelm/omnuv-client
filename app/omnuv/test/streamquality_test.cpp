// A runnable check for app/omnuv/streamquality.cpp, and the only one there is.
//
// This fork has no test harness, and adding one for Qt and SDL would be more
// machinery than the thing it tests. So the file under test is compiled with
// its real header and moonlight's real decoder.h, against ~120 lines of stubs
// in test/stub/ that stand in for QString, SDL and Session -- which is enough,
// because the grading ladder touches none of them except to read a struct and
// write a string.
//
//     g++ -std=c++17 -Wall -Wformat -I app/omnuv/test/stub -I app
//         -o /tmp/sqt app/omnuv/test/streamquality_test.cpp && /tmp/sqt
//
// It is not in app.pro and never ships: that file lists its sources one by one.
// `app/omnuv/test/checks.sh` builds and runs it.
//
// Everything worth testing is in the anonymous namespace of the file under
// test, which is why it is included as source rather than linked. What the
// assertions are actually for, in order of how expensive the bug would be:
//
//   an unknown reading must never render as a zero -- a zero is a pass, and
//   `gaming-rig-e2e` would read it as a perfect score;
//   the frame-rate bar must be a ratio -- the lab's absolute 55 fps is
//   unreachable at 30 and met while half the frames are gone at 120;
//   frames stopping must be a state and not a failure;
//   and a threshold typo must not become zero, which grades everything red.

#include <cassert>
#include <cstring>
#include "SDL_compat.h"
#include "streaming/session.h"

Uint32 g_fakeTicks = 0;
char g_lastLog[1024] = {};
bool g_fakeMotion = true;
static Session s_session;
Session* Session::get() { return &s_session; }

#include "omnuv/streamquality.cpp"

static VIDEO_STATS perfect(int fps)
{
    VIDEO_STATS s = {};
    s.receivedFps = s.decodedFps = s.renderedFps = s.totalFps = fps;
    s.totalFrames = s.receivedFrames = s.decodedFrames = s.renderedFrames = fps * 2;
    s.framesWithHostProcessingLatency = fps * 2;
    s.totalHostProcessingLatency = 30 * fps * 2;   // 3.0 ms each, in 1/10 ms units
    s.minHostProcessingLatency = 20; s.maxHostProcessingLatency = 40;
    s.lastRtt = 3; s.lastRttVariance = 1;
    return s;
}

static const char* mark()
{
    return s_session.m_o.enabled[Overlay::OverlayStatusUpdate]
         ? s_session.m_o.text[Overlay::OverlayStatusUpdate] : "";
}

// Frames keep arriving: the decoder calls onStats() once a second. A test that
// advances the clock without it is testing the "no picture" path instead.
static void run(const VIDEO_STATS& s, int seconds, bool slotBusy = false)
{
    for (int i = 0; i < seconds; i++) {
        OmnuvStreamQuality::onStats(s);
        g_fakeTicks += 1000;
        OmnuvStreamQuality::tick(slotBusy);
    }
}
// Past the three-second settle window moonlight-common-c also observes.
static void settle(const VIDEO_STATS& s, bool slotBusy = false) { run(s, 4, slotBusy); }

static void start(int fps, int kbps, bool warnings = true)
{
    OmnuvStreamQuality::end();
    OmnuvStreamQuality::begin(QString("gpu-workstation"), fps, kbps, warnings);
}

int main()
{
    // ---- thresholds: defaults, the knob, and a typo -----------------------
    {
        Thresholds d = OmnuvStreamQuality::thresholdsFromEnvironment(nullptr);
        assert(d.rttCautionMs == 40.0 && d.lossCriticalPct == 5.0);

        Thresholds t = OmnuvStreamQuality::thresholdsFromEnvironment("rtt_caution=120, loss_caution=3.5");
        assert(t.rttCautionMs == 120.0 && t.lossCautionPct == 3.5);
        assert(t.rttCriticalMs == 80.0);              // untouched keeps its default

        // A typo must not become zero: zero grades everything critical.
        Thresholds bad = OmnuvStreamQuality::thresholdsFromEnvironment("rtt_caution=fast,nonsense=1,noequals");
        assert(bad.rttCautionMs == 40.0);
    }

    // ---- 60 fps, healthy: nothing draws -----------------------------------
    start(60, 20000);
    settle(perfect(60));
    assert(strcmp(mark(), "") == 0);
    assert(strstr(g_lastLog, "verdict=good") && strstr(g_lastLog, "cause=-"));

    // ---- the same shortfall at 30 and at 120 fps --------------------------
    // 29 of 30 is 96.7%: good. The lab's absolute "55 fps" would have called
    // it a permanent failure. 59 of 120 is 49%: critical. The same absolute
    // would have called it a pass. This is the honesty question the brief asks.
    start(30, 20000);
    { VIDEO_STATS s = perfect(30); s.receivedFps = 29; settle(s); }
    assert(strcmp(mark(), "") == 0);

    start(120, 20000);
    { VIDEO_STATS s = perfect(120); s.receivedFps = 59; settle(s); }
    assert(strstr(mark(), "frame rate"));
    assert(strstr(g_lastLog, "verdict=critical"));

    // ---- packet loss draws amber, with the cause and upstream's advice ----
    start(60, 20000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 4; settle(s); }  // 4/120 = 3.3%
    assert(strstr(mark(), "packet loss"));
    assert(strstr(mark(), "Try a lower bitrate"));    // > 5 Mbps, upstream's own condition
    assert(strstr(mark(), "[|||."));                  // one caution, one bar gone

    start(60, 4000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 4; settle(s); }
    assert(strstr(mark(), "packet loss") && !strstr(mark(), "bitrate"));

    // ---- unknown is never a pass and never a mark -------------------------
    start(60, 20000);
    { VIDEO_STATS s = perfect(60);
      s.framesWithHostProcessingLatency = 0; s.totalHostProcessingLatency = 0;  // host silent
      s.lastRtt = 0;                                                            // ENet cannot say
      settle(s); }
    assert(strcmp(mark(), "") == 0);
    assert(strstr(g_lastLog, "host=na") && strstr(g_lastLog, "rtt=na"));
    assert(!strstr(g_lastLog, "host=0.00"));

    // ---- nothing gradeable at all: unknown, not good ----------------------
    start(60, 20000);
    { VIDEO_STATS s = {}; settle(s); }
    assert(strstr(g_lastLog, "verdict=unknown"));
    assert(strcmp(mark(), "") == 0);

    // ---- the host's own verdict is a floor --------------------------------
    start(60, 20000);
    settle(perfect(60));
    OmnuvStreamQuality::onHostVerdict(CONN_STATUS_POOR);
    run(perfect(60), 1);
    assert(strstr(g_lastLog, "verdict=critical"));
    assert(strstr(mark(), "packet loss"));
    OmnuvStreamQuality::onHostVerdict(CONN_STATUS_OKAY);
    run(perfect(60), 1);
    assert(strstr(g_lastLog, "verdict=good"));
    assert(strcmp(mark(), "") == 0);

    // ---- frames stop: a state, not a failure ------------------------------
    start(60, 20000);
    settle(perfect(60));
    assert(strcmp(mark(), "") == 0);
    g_fakeTicks += 3000;                      // three seconds with no frame
    OmnuvStreamQuality::tick(false);
    assert(strstr(mark(), "Reconnecting to gpu-workstation"));
    assert(strstr(mark(), "Ctrl+Alt+Shift+Q"));
    assert(strstr(g_lastLog, "verdict=waiting"));
    assert(!OmnuvStreamQuality::lostPictureSentence().isEmpty());

    // ...and it lifts when frames come back, rather than terminating
    run(perfect(60), 1);
    assert(strcmp(mark(), "") == 0);
    assert(strstr(g_lastLog, "verdict=good"));

    // ---- a healthy stream leaves upstream's own wording alone -------------
    start(60, 20000);
    settle(perfect(60));
    assert(OmnuvStreamQuality::lostPictureSentence().isEmpty());

    // ---- the mouse-emulation indicator keeps the slot ---------------------
    start(60, 20000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 20; settle(s, true); }
    assert(strcmp(mark(), "") == 0);
    assert(strstr(g_lastLog, "verdict=critical"));   // the log still says so

    // ...and the mark comes back when the indicator gives the slot up, which
    // it does by turning the overlay off underneath us.
    s_session.m_o.setOverlayState(Overlay::OverlayStatusUpdate, false);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 20; run(s, 1); }
    assert(strstr(mark(), "packet loss"));

    // ---- connectionWarnings off: no mark, log unaffected ------------------
    start(60, 20000, false);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 20; settle(s); }
    assert(strcmp(mark(), "") == 0);
    assert(strstr(g_lastLog, "verdict=critical"));

    // ---- reduced motion: a mark that is up does not blink off -------------
    g_fakeMotion = false;
    start(60, 20000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 4; settle(s); }
    assert(strstr(mark(), "packet loss"));
    run(perfect(60), 1);                                 // recovered
    assert(strstr(mark(), "packet loss"));               // held, not strobed
    run(perfect(60), 3);
    assert(strcmp(mark(), "") == 0);                     // and then cleared
    g_fakeMotion = true;

    // ...and with motion on it clears at once
    start(60, 20000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 4; settle(s); }
    assert(strstr(mark(), "packet loss"));
    run(perfect(60), 1);
    assert(strcmp(mark(), "") == 0);

    // ---- the panel: grouped, and it says unknown out loud -----------------
    start(60, 20000);
    s_session.m_o.setOverlayState(Overlay::OverlayDebug, true);
    { VIDEO_STATS s = perfect(60); s.framesWithHostProcessingLatency = 0; s.lastRtt = 0;
      settle(s); }
    {
        const char* p = s_session.m_o.text[Overlay::OverlayDebug];
        assert(strstr(p, "VIDEO") && strstr(p, "NETWORK") && strstr(p, "HOST"));
        assert(strstr(p, "round trip      unknown"));
        assert(strstr(p, "produce frame   unknown"));
        fprintf(stderr, "\n--- panel ---\n%s", p);
    }

    // ---- two criticals: the cause names the worse of them -----------------
    // 20 of 120 frames is 16.7% against a 5% bar; 95 ms is against an 80 ms
    // bar. Both critical, and loss is four times past its bar while latency is
    // barely past its own -- so the one word had better be "packet loss".
    start(60, 20000);
    { VIDEO_STATS s = perfect(60); s.networkDroppedFrames = 20; s.lastRtt = 95; settle(s); }
    assert(strstr(mark(), "packet loss"));
    assert(strstr(g_lastLog, "cause=packet loss"));
    fprintf(stderr, "\n--- mark ---\n%s\n", mark());
    g_fakeTicks += 4000; OmnuvStreamQuality::tick(false);
    fprintf(stderr, "\n--- waiting ---\n%s\n", mark());
    fprintf(stderr, "\n--- sentence ---\n%s\n", OmnuvStreamQuality::lostPictureSentence().s.c_str());
    fprintf(stderr, "\n--- log ---\n%s\n", g_lastLog);

    fprintf(stderr, "\nall stream-quality assertions passed\n");
    return 0;
}
