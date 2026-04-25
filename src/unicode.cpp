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

std::string utf8_grapheme_at(const std::string& str, int index) {
    auto gs = utf8_graphemes(str);
    if (index < 0) index += static_cast<int>(gs.size());
    if (index < 0 || index >= static_cast<int>(gs.size())) return "";
    return gs[index];
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

            // Lowercase remaining codepoints in this grapheme
            ssize_t pos = first_bytes;
            while (pos < glen) {
                int32_t cp;
                ssize_t bytes = utf8proc_iterate(data + pos, glen - pos, &cp);
                if (bytes < 1) { result += g[pos]; pos++; continue; }
                int32_t low = utf8proc_tolower(cp);
                enc = utf8proc_encode_char(low, buf);
                result.append(reinterpret_cast<char*>(buf), enc);
                pos += bytes;
            }
            cap_next = false;
        } else {
            // Lowercase the entire grapheme
            const uint8_t* d = reinterpret_cast<const uint8_t*>(g.data());
            ssize_t p = 0;
            while (p < glen) {
                int32_t cp;
                ssize_t bytes = utf8proc_iterate(d + p, glen - p, &cp);
                if (bytes < 1) { result += g[p]; p++; continue; }
                int32_t low = utf8proc_tolower(cp);
                uint8_t buf2[4];
                ssize_t enc2 = utf8proc_encode_char(low, buf2);
                result.append(reinterpret_cast<char*>(buf2), enc2);
                p += bytes;
            }
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

int utf8_byte_to_grapheme_index(const std::string& str, size_t byte_offset) {
    if (str.empty() || byte_offset == std::string::npos) return -1;

    // Walk grapheme clusters, track their starting byte offsets
    auto gs = utf8_graphemes(str);
    size_t byte_pos = 0;
    for (int i = 0; i < static_cast<int>(gs.size()); i++) {
        if (byte_pos == byte_offset) return i;
        byte_pos += gs[i].size();
    }
    // byte_offset at end of string
    if (byte_pos == byte_offset) return static_cast<int>(gs.size());
    // byte_offset falls mid-grapheme — return the containing grapheme
    byte_pos = 0;
    for (int i = 0; i < static_cast<int>(gs.size()); i++) {
        if (byte_offset >= byte_pos && byte_offset < byte_pos + gs[i].size())
            return i;
        byte_pos += gs[i].size();
    }
    return -1;
}

#endif
