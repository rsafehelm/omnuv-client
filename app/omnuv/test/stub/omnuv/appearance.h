// Stub for streamquality_test.cpp -- the reduced-motion reading, as a variable the test sets.
#pragma once
extern bool g_fakeMotion;
class OmnuvAppearance { public: static bool animationsEnabledNow() { return g_fakeMotion; } };
