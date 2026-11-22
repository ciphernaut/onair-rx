// SPDX-License-Identifier: GPL-3.0-or-later
// onair-rx: the JSON writer of the lines (rx.h). Based on OnAir by Kristijonas Adomaitis - https://on-air.dev
//
// A file of its own so that the tests' generator (test/gen.cpp) can link the pieces it tests (ais.cpp) without main.cpp.
#include "rx.h"
#include <cmath>
#include <cstdio>

namespace onair_rx {

namespace {
void escape(std::string& o, const std::string& v) {
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20 || c >= 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
}

// v is well-formed UTF-8 (RFC 3629): no stray continuation byte, no overlong form, no surrogate, nothing above U+10FFFF
bool validUtf8(const std::string& v) {
    const auto* p = (const unsigned char*)v.data();
    const size_t n = v.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t len;
        uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return false;   // a continuation byte first, an overlong 2-byte lead (0xC0, 0xC1), or past U+10FFFF
        if (i + len > n) return false;
        for (size_t k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = cp << 6 | (p[i + k] & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}
} // namespace

Json& Json::utf8(const char* k, const std::string& v) {
    if (!validUtf8(v)) return str(k, v);
    key(k);
    s_ += '"';
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') { s_ += '\\'; s_ += (char)c; }
        else if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); s_ += b; }
        else s_ += (char)c;
    }
    s_ += '"';
    return *this;
}

void Json::key(const char* k) {
    if (s_.size() > 1) s_ += ',';
    s_ += '"'; escape(s_, k); s_ += "\":";
    keys_.emplace_back(k);
}
bool Json::has(const std::string& k) const {
    for (const std::string& h : keys_) if (h == k) return true;
    return false;
}
Json& Json::arr(const char* k, const std::vector<std::string>& v) {
    key(k);
    s_ += '[';
    for (size_t i = 0; i < v.size(); i++) { if (i) s_ += ','; s_ += '"'; escape(s_, v[i]); s_ += '"'; }
    s_ += ']';
    return *this;
}
Json& Json::raw(const char* k, const std::string& json) { key(k); s_ += json; return *this; }
Json& Json::str(const char* k, const std::string& v) { key(k); s_ += '"'; escape(s_, v); s_ += '"'; return *this; }
Json& Json::num(const char* k, long long v) { key(k); s_ += std::to_string(v); return *this; }
Json& Json::num(const char* k, double v, int decimals) {
    if (!std::isfinite(v)) return *this;   // not a value JSON can hold: the member is left out
    char b[48];
    snprintf(b, sizeof b, "%.*f", decimals, v);
    key(k); s_ += b; return *this;
}
Json& Json::flag(const char* k, bool v) { key(k); s_ += v ? "true" : "false"; return *this; }

} // namespace onair_rx
