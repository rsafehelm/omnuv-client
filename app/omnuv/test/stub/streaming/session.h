// Stub for streamquality_test.cpp -- Session and OverlayManager: the two overlay slots as plain buffers, so a test can read what a person would have seen.
#pragma once
#include "SDL_compat.h"
namespace Overlay {
enum OverlayType { OverlayDebug, OverlayStatusUpdate, OverlayMax };
class OverlayManager {
public:
    bool enabled[OverlayMax] = {};
    char text[OverlayMax][1024] = {};
    SDL_Color colour[OverlayMax] = {};
    bool isOverlayEnabled(OverlayType t) { return enabled[t]; }
    void updateOverlayText(OverlayType t, const char* s) { SDL_strlcpy(text[t], s, sizeof(text[t])); }
    void setOverlayState(OverlayType t, bool e) { enabled[t] = e; }
    void setOverlayColor(OverlayType t, SDL_Color c) { colour[t] = c; }
};
}
class Session { public:
    static Session* get(); Overlay::OverlayManager& getOverlayManager() { return m_o; }
    Overlay::OverlayManager m_o; };
