#pragma once

#include <string_view>

/** @brief Extract a word from the beginning of content, never reading over newlines.
 * Also removes any whitespace in the returned word and in the changed content. */
std::string_view extract_word(std::string_view &content, char delim = ' ') {
	if (content.size() >= 2 && content[0] == '\r' && content[1] == '\n')
		return {};
	auto start_word = content.find_first_not_of(delim);
	if (start_word == std::string_view::npos) start_word = 0;
	char ends[]{" \r\n"};
	ends[0] = delim;
	auto end_word = content.find_first_of(ends, start_word);
	auto ret = content.substr(start_word, end_word - start_word);
	auto s = content.find_first_not_of(delim, end_word);
	if (s == std::string_view::npos)
		content = {};
	else
		content = content.substr(s);
	return ret;
}
/** @brief Extract until newline */
std::string_view extract_until_newline(std::string_view &content) {
	if (content.size() >= 2 && content[0] == '\r' && content[1] == '\n')
		return {};
	auto start_word = content.find_first_not_of(' ');
	if (start_word == std::string_view::npos) start_word = 0;
	auto end_word = content.find_first_of("\r\n", start_word);
	auto ret = content.substr(start_word, end_word - start_word);
	auto s = end_word;
	if (s == std::string_view::npos)
		content = {};
	else
		content = content.substr(s);
	return ret;
}
/** @brief Extract a newline including carriage return.
 *  @return bool with false if no newline sequence was found at the beginning of content,
 *  in which case content was not modified*/
bool extract_newline(std::string_view &content) {
	if (content.size() < 2)
		return false;
	if (content[0] != '\r' || content[1] != '\n')
		return false;
	content = content.substr(2);
	return true;
}

constexpr void skip_whitespace(std::string_view &content) {
	content = content.substr(std::min(content.size(), content.find_first_not_of(" \t\n\v\r\f")));
}

constexpr bool is_quote(char c) { return c == '"' || c == '\''; }
