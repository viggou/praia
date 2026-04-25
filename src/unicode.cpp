#include "unicode.h"

// Always available: manual UTF-8 encoding for a single codepoint
std::string utf8_encode(int32_t cp) {
    std::string result;
    if (cp < 0x80) {
        result += static_cast<char>(cp);
    } else if (cp < 0x800) {
        result += static_cast<char>(0xC0 | (cp >> 6));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        result += static_cast<char>(0xE0 | (cp >> 12));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF) {
        result += static_cast<char>(0xF0 | (cp >> 18));
        result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return result;
}

#ifdef HAVE_UTF8PROC

#include <utf8proc.h>

// Split a UTF-8 string into grapheme cluster substrings
std::vector<std::string> utf8_graphemes(const std::string& str) {
    std::vector<std::string> result;
    if (str.empty()) return result;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    ssize_t cluster_start = 0;

    int32_t prev_cp = -1;
    int32_t state = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; } // skip invalid byte

        if (prev_cp >= 0 && utf8proc_grapheme_break_stateful(prev_cp, cp, &state)) {
            result.push_back(str.substr(cluster_start, pos - cluster_start));
            cluster_start = pos;
        }

        prev_cp = cp;
        pos += bytes;
    }

    // Last cluster
    if (cluster_start < len)
        result.push_back(str.substr(cluster_start));

    return result;
}

size_t utf8_grapheme_count(const std::string& str) {
    if (str.empty()) return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    size_t count = 0;

    int32_t prev_cp = -1;
    int32_t state = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; }

        if (prev_cp >= 0 && utf8proc_grapheme_break_stateful(prev_cp, cp, &state))
            count++;

        prev_cp = cp;
        pos += bytes;
    }

    if (prev_cp >= 0) count++; // count the last cluster
    return count;
}

// Walk grapheme clusters, returning the nth one directly without allocating a full vector.
// Negative indexing is handled by the caller (pre-resolved via utf8_grapheme_count).
std::string utf8_grapheme_at(const std::string& str, int index) {
    if (str.empty() || index < 0) return "";

    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    ssize_t cluster_start = 0;
    int cluster_idx = 0;

    int32_t prev_cp = -1;
    int32_t state = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; }

        if (prev_cp >= 0 && utf8proc_grapheme_break_stateful(prev_cp, cp, &state)) {
            if (cluster_idx == index)
                return str.substr(cluster_start, pos - cluster_start);
            cluster_idx++;
            cluster_start = pos;
        }

        prev_cp = cp;
        pos += bytes;
    }

    // Last cluster
    if (cluster_idx == index && cluster_start < len)
        return str.substr(cluster_start);

    return "";
}

std::string utf8_upper(const std::string& str) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    std::string result;
    result.reserve(str.size());

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { result += str[pos]; pos++; continue; }

        int32_t upper = utf8proc_toupper(cp);
        uint8_t buf[4];
        ssize_t enc = utf8proc_encode_char(upper, buf);
        result.append(reinterpret_cast<char*>(buf), enc);
        pos += bytes;
    }
    return result;
}

std::string utf8_lower(const std::string& str) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    std::string result;
    result.reserve(str.size());

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { result += str[pos]; pos++; continue; }

        int32_t lower = utf8proc_tolower(cp);
        uint8_t buf[4];
        ssize_t enc = utf8proc_encode_char(lower, buf);
        result.append(reinterpret_cast<char*>(buf), enc);
        pos += bytes;
    }
    return result;
}

std::string utf8_title(const std::string& str) {
    auto graphemes_list = utf8_graphemes(str);
    std::string result;
    result.reserve(str.size());
    bool cap_next = true;

    for (auto& g : graphemes_list) {
        if (g.empty()) continue;

        // Check if grapheme starts with whitespace (ASCII whitespace is single byte)
        const uint8_t* data = reinterpret_cast<const uint8_t*>(g.data());
        ssize_t glen = static_cast<ssize_t>(g.size());
        int32_t first_cp;
        ssize_t first_bytes = utf8proc_iterate(data, glen, &first_cp);

        if (first_bytes < 1) {
            result += g;
            continue;
        }

        // Check whitespace via Unicode category
        utf8proc_category_t cat = utf8proc_category(first_cp);
        bool is_space = (cat == UTF8PROC_CATEGORY_ZS || first_cp == '\t' ||
                         first_cp == '\n' || first_cp == '\r' || first_cp == ' ');

        if (is_space) {
            result += g;
            cap_next = true;
        } else if (cap_next) {
            // Uppercase first codepoint, lowercase rest
            int32_t upper = utf8proc_toupper(first_cp);
            uint8_t buf[4];
            ssize_t enc = utf8proc_encode_char(upper, buf);
            result.append(reinterpret_cast<char*>(buf), enc);
            if (first_bytes < glen)
                result += utf8_lower(g.substr(first_bytes));
            cap_next = false;
        } else {
            result += utf8_lower(g);
        }
    }
    return result;
}

int32_t utf8_first_codepoint(const std::string& str) {
    if (str.empty()) return -1;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    int32_t cp;
    ssize_t bytes = utf8proc_iterate(data, static_cast<ssize_t>(str.size()), &cp);
    if (bytes < 1) return -1;
    return cp;
}

std::string utf8_from_codepoint(int32_t cp) {
    uint8_t buf[4];
    ssize_t len = utf8proc_encode_char(cp, buf);
    if (len < 1) return "";
    return std::string(reinterpret_cast<char*>(buf), len);
}

std::vector<int32_t> utf8_codepoints(const std::string& str) {
    std::vector<int32_t> result;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; }
        result.push_back(cp);
        pos += bytes;
    }
    return result;
}

size_t utf8_first_grapheme_bytes(const std::string& str) {
    if (str.empty()) return 0;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;

    int32_t prev_cp = -1;
    int32_t state = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; }

        if (prev_cp >= 0 && utf8proc_grapheme_break_stateful(prev_cp, cp, &state))
            return static_cast<size_t>(pos); // break before this codepoint

        prev_cp = cp;
        pos += bytes;
    }

    return str.size(); // entire string is one grapheme
}

int utf8_byte_to_grapheme_index(const std::string& str, size_t byte_offset) {
    if (str.empty() || byte_offset == std::string::npos) return -1;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(str.data());
    ssize_t len = static_cast<ssize_t>(str.size());
    ssize_t pos = 0;
    ssize_t cluster_start = 0;
    int grapheme_idx = 0;

    int32_t prev_cp = -1;
    int32_t state = 0;

    while (pos < len) {
        int32_t cp;
        ssize_t bytes = utf8proc_iterate(data + pos, len - pos, &cp);
        if (bytes < 1) { pos++; continue; }

        if (prev_cp >= 0 && utf8proc_grapheme_break_stateful(prev_cp, cp, &state)) {
            // byte_offset falls within the previous cluster
            if (byte_offset >= static_cast<size_t>(cluster_start) &&
                byte_offset < static_cast<size_t>(pos))
                return grapheme_idx;
            grapheme_idx++;
            cluster_start = pos;
        }

        prev_cp = cp;
        pos += bytes;
    }

    // Check if byte_offset falls in the last cluster
    if (byte_offset >= static_cast<size_t>(cluster_start) &&
        byte_offset < static_cast<size_t>(pos))
        return grapheme_idx;

    return -1;
}

#endif
