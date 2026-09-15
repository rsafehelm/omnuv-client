// Stub for streamquality_test.cpp -- SDL: types, the two atomics, the string helpers, a clock the test drives, and a log that remembers its last line so an assertion can read it.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
typedef uint8_t Uint8; typedef uint32_t Uint32;
struct SDL_Color { Uint8 r, g, b, a; };
typedef int SDL_SpinLock;
typedef struct { int value; } SDL_atomic_t;
inline void SDL_AtomicLock(SDL_SpinLock*) {}
inline void SDL_AtomicUnlock(SDL_SpinLock*) {}
inline int SDL_AtomicSet(SDL_atomic_t* a, int v) { int o = a->value; a->value = v; return o; }
inline int SDL_AtomicGet(SDL_atomic_t* a) { return a->value; }
inline int SDL_memcmp(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
inline int SDL_strcmp(const char* a, const char* b) { return strcmp(a, b); }
inline size_t SDL_strlcpy(char* d, const char* s, size_t n) {
    size_t l = strlen(s); if (n) { size_t c = l >= n ? n - 1 : l; memcpy(d, s, c); d[c] = 0; } return l; }
inline char* SDL_getenv(const char* n) { return getenv(n); }
extern Uint32 g_fakeTicks;
inline Uint32 SDL_GetTicks() { return g_fakeTicks; }
#define SDL_LOG_CATEGORY_APPLICATION 0
extern char g_lastLog[1024];
inline void SDL_LogInfo(int, const char* fmt, const char* a) { snprintf(g_lastLog, sizeof(g_lastLog), fmt, a); fprintf(stderr, "%s\n", g_lastLog); }
typedef struct SDL_Window SDL_Window;
