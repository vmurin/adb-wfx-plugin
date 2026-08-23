// UTF-16 <-> UTF-8 conversion and FILETIME <-> time_t arithmetic. This is
// the boundary layer between the SDK's UTF-16 WCHAR*/FILETIME world and the
// rest of the plugin, which works exclusively in UTF-8 std::string and
// POSIX time_t. Only fsplugin.cpp (the export boundary) should call these.
//
// Hand-rolled rather than std::wstring_convert (deprecated, removed in
// newer toolchains) or iconv (external dependency). Malformed input is
// never fatal: an unpaired surrogate, a truncated or overlong UTF-8
// sequence, an invalid lead byte, or a code point above U+10FFFF all
// become U+FFFD (the replacement character) and decoding continues.
#ifndef ADB_WFX_UTILS_HPP
#define ADB_WFX_UTILS_HPP

#include "sdk.h"

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

constexpr uint32_t UNICODE_REPLACEMENT_CHARACTER = 0xFFFD;
constexpr uint32_t UNICODE_MAX_CODE_POINT = 0x10FFFF;

constexpr uint64_t FILETIME_TICKS_PER_SECOND = 10000000ULL;
constexpr uint64_t FILETIME_AT_UNIX_EPOCH = 116444736000000000ULL;

// Appends the UTF-8 encoding of a single Unicode code point to out.
inline void appendUtf8(std::string& out, uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

// Appends the UTF-16 encoding of a single Unicode code point to out,
// producing a surrogate pair for code points above U+FFFF.
inline void appendWide(std::vector<WCHAR>& out, uint32_t codepoint) {
    if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<WCHAR>(codepoint));
    } else {
        uint32_t v = codepoint - 0x10000;
        out.push_back(static_cast<WCHAR>(0xD800 + (v >> 10)));
        out.push_back(static_cast<WCHAR>(0xDC00 + (v & 0x3FF)));
    }
}

// Decodes one UTF-8 sequence starting at src[pos]. On success returns the
// code point and the number of bytes it occupied. On any malformed input
// (invalid lead byte, truncated sequence, overlong encoding, encoded
// surrogate, or code point above U+10FFFF) returns the replacement
// character and advances by exactly 1 byte, so the caller always makes
// progress and never reads past the end of src.
struct Utf8Decode {
    uint32_t codepoint;
    size_t consumed;
};

inline Utf8Decode decodeUtf8(const std::string& src, size_t pos) {
    unsigned char lead = static_cast<unsigned char>(src[pos]);
    if (lead <= 0x7F) {
        return {lead, 1};
    }

    size_t seqLen;
    uint32_t codepoint;
    uint32_t minCodepoint;
    if ((lead & 0xE0) == 0xC0) {
        seqLen = 2;
        codepoint = lead & 0x1F;
        minCodepoint = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        seqLen = 3;
        codepoint = lead & 0x0F;
        minCodepoint = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        seqLen = 4;
        codepoint = lead & 0x07;
        minCodepoint = 0x10000;
    } else {
        return {UNICODE_REPLACEMENT_CHARACTER, 1}; // invalid lead byte
    }

    if (src.size() - pos < seqLen) {
        return {UNICODE_REPLACEMENT_CHARACTER, 1}; // truncated at end of string
    }

    for (size_t k = 1; k < seqLen; ++k) {
        unsigned char cont = static_cast<unsigned char>(src[pos + k]);
        if ((cont & 0xC0) != 0x80) {
            return {UNICODE_REPLACEMENT_CHARACTER, 1}; // broken continuation byte
        }
        codepoint = (codepoint << 6) | (cont & 0x3F);
    }

    if (codepoint < minCodepoint || codepoint > UNICODE_MAX_CODE_POINT ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return {UNICODE_REPLACEMENT_CHARACTER, seqLen}; // overlong, too large, or a surrogate
    }

    return {codepoint, seqLen};
}

inline std::string wideToUtf8(const WCHAR* src, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        WCHAR unit = src[i];
        uint32_t codepoint;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            WCHAR next = (i + 1 < len) ? src[i + 1] : 0;
            if (next >= 0xDC00 && next <= 0xDFFF) {
                codepoint = 0x10000 +
                    ((static_cast<uint32_t>(unit) - 0xD800) << 10) +
                    (static_cast<uint32_t>(next) - 0xDC00);
                ++i; // consumed the low surrogate too
            } else {
                codepoint = UNICODE_REPLACEMENT_CHARACTER; // unpaired high surrogate
            }
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            codepoint = UNICODE_REPLACEMENT_CHARACTER; // unpaired low surrogate
        } else {
            codepoint = unit;
        }
        appendUtf8(out, codepoint);
    }
    return out;
}

inline std::string wideToUtf8(const WCHAR* src) {
    size_t len = 0;
    while (src[len] != 0) {
        ++len;
    }
    return wideToUtf8(src, len);
}

inline std::vector<WCHAR> utf8ToWide(const std::string& src) {
    std::vector<WCHAR> out;
    size_t pos = 0;
    while (pos < src.size()) {
        Utf8Decode decoded = decodeUtf8(src, pos);
        appendWide(out, decoded.codepoint);
        pos += decoded.consumed;
    }
    out.push_back(0);
    return out;
}

// Copies at most (destChars-1) UTF-16 code units plus a NUL terminator into
// dest. Never splits a surrogate pair: if the truncation point would land
// between a high and low surrogate, the whole pair is dropped instead.
// Returns false if the source had to be truncated to fit.
inline bool utf8ToWideBuf(const std::string& src, WCHAR* dest, size_t destChars) {
    if (destChars == 0) {
        return false;
    }

    std::vector<WCHAR> wide = utf8ToWide(src); // includes its own trailing 0
    size_t sourceUnits = wide.size() - 1;
    size_t maxUnits = destChars - 1;

    if (sourceUnits <= maxUnits) {
        for (size_t i = 0; i < sourceUnits; ++i) {
            dest[i] = wide[i];
        }
        dest[sourceUnits] = 0;
        return true;
    }

    size_t copyUnits = maxUnits;
    if (copyUnits > 0) {
        WCHAR last = wide[copyUnits - 1];
        if (last >= 0xD800 && last <= 0xDBFF) {
            --copyUnits; // would split a surrogate pair; drop the lead half too
        }
    }
    for (size_t i = 0; i < copyUnits; ++i) {
        dest[i] = wide[i];
    }
    dest[copyUnits] = 0;
    return false;
}

inline FILETIME timeToFileTime(time_t t) {
    uint64_t ticks = FILETIME_AT_UNIX_EPOCH +
        static_cast<uint64_t>(t) * FILETIME_TICKS_PER_SECOND;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
}

inline time_t fileTimeToTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) {
        return 0; // a zero FILETIME means "unknown" in this SDK
    }
    uint64_t ticks = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    uint64_t seconds = (ticks - FILETIME_AT_UNIX_EPOCH) / FILETIME_TICKS_PER_SECOND;
    return static_cast<time_t>(seconds);
}

#endif // ADB_WFX_UTILS_HPP
