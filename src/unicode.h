#pragma once
#include <string>
#include <vector>
#include <cstdint>

#ifdef HAVE_UTF8PROC

size_t utf8_grapheme_count(const std::string& str);
std::string utf8_grapheme_at(const std::string& str, int index);
std::vector<std::string> utf8_graphemes(const std::string& str);

std::string utf8_upper(const std::string& str);
std::string utf8_lower(const std::string& str);
std::string utf8_title(const std::string& str);

int32_t utf8_first_codepoint(const std::string& str);
std::string utf8_from_codepoint(int32_t cp);
std::vector<int32_t> utf8_codepoints(const std::string& str);

// Byte length of the first grapheme cluster (0 if empty)
size_t utf8_first_grapheme_bytes(const std::string& str);

// Convert byte offset from str.find() to grapheme cluster index
int utf8_byte_to_grapheme_index(const std::string& str, size_t byte_offset);

#endif

// Always available: manual UTF-8 encoding (used by lexer without HAVE_UTF8PROC)
std::string utf8_encode(int32_t cp);
