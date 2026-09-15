// Just enough Qt for app/omnuv/streamquality.cpp to compile and run outside a
// Qt build. Not a Qt implementation and not trying to be: every method here
// exists because that one file calls it. If it grows, the file under test has
// started depending on Qt for something it should not.
#pragma once
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cmath>
struct QByteArray {
    std::string s;
    QByteArray() {}
    QByteArray(const char* p) : s(p ? p : "") {}
    QByteArray(const std::string& v) : s(v) {}
    const char* constData() const { return s.c_str(); }
    bool isEmpty() const { return s.empty(); }
    int indexOf(char c) const { auto p = s.find(c); return p == std::string::npos ? -1 : (int)p; }
    QByteArray left(int n) const { return QByteArray(s.substr(0, n)); }
    QByteArray mid(int n) const { return QByteArray(n >= (int)s.size() ? std::string() : s.substr(n)); }
    QByteArray trimmed() const {
        size_t a = s.find_first_not_of(" \t\n"); if (a == std::string::npos) return QByteArray();
        size_t b = s.find_last_not_of(" \t\n"); return QByteArray(s.substr(a, b - a + 1));
    }
    std::vector<QByteArray> split(char c) const {
        std::vector<QByteArray> out; std::string cur;
        for (char ch : s) { if (ch == c) { out.push_back(QByteArray(cur)); cur.clear(); } else cur += ch; }
        out.push_back(QByteArray(cur)); return out;
    }
    double toDouble(bool* ok) const { char* e = nullptr; double v = strtod(s.c_str(), &e);
        *ok = e && *e == '\0' && !s.empty(); return v; }
    bool operator==(const char* p) const { return s == p; }
};
template<class T> using QList = std::vector<T>;
struct QString {
    std::string s;
    QString() {}
    QString(const char* p) : s(p ? p : "") {}
    QString(const std::string& v) : s(v) {}
    int length() const { return (int)s.size(); }
    bool isEmpty() const { return s.empty(); }
    QString left(int n) const { return QString(s.substr(0, n)); }
    QString operator+(const QString& o) const { return QString(s + o.s); }
    QByteArray toUtf8() const { return QByteArray(s); }
    QString arg(const QString& v) const { auto p = s.find("%1"); return p==std::string::npos ? *this
        : QString(s.substr(0,p) + v.s + s.substr(p+2)); }
    QString arg(int v) const { char b[32]; snprintf(b,sizeof b,"%d",v);
        auto p = s.find("%2"); if (p==std::string::npos) p = s.find("%1");
        return p==std::string::npos ? *this : QString(s.substr(0,p) + b + s.substr(p+2)); }
};
struct QObject { static QString tr(const char* t) { return QString(t); } };
inline int qRound(double d) { return (int)(d < 0 ? d - 0.5 : d + 0.5); }
#define qWarning(...) do { fprintf(stderr, "qWarning: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
struct QSize { int w = 0, h = 0; };
