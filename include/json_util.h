#pragma once

#include <optional>

#include "string_util.h"

#define JSON_ASSERT(x, message) if (!(x)) {LogError(message); return {}; }
constexpr bool parse_remove_json_obj_start(std::string_view &json) {
	JSON_ASSERT(json.size() && json[0] == '{', "Invalid json");
	json = json.substr(1);
	return true;
}
constexpr bool try_parse_remove_json_obj_end(std::string_view &json) {
	bool is_end = json.size() && json[0] == '}';
	if (!is_end)
		return false;
	json = json.substr(1);
	return true;
}
constexpr bool try_parse_remove_json_sep(std::string_view &json) {
	bool is_sep = json.size() && json[0] == ',';
	if (!is_sep)
		return false;
	json = json.substr(1);
	return true;
}
constexpr std::optional<std::string_view> parse_remove_json_string(std::string_view &json) {
	skip_whitespace(json);
	JSON_ASSERT(json.size() && is_quote(json[0]), "Missing start quote string");
	auto end = std::min(json.find_first_of("\"'", 1), json.size());
	std::string_view s = json.substr(1, end - 1);
	json = json.substr(end);
	JSON_ASSERT(json.size() && is_quote(json[0]), "Missing end quote for string");
	json = json.substr(1);
	skip_whitespace(json);
	return s;
}
constexpr std::optional<std::string_view> parse_remove_json_key(std::string_view &json) {
	auto key = parse_remove_json_string(json);
	JSON_ASSERT(key, "Error parsing the key");
	JSON_ASSERT(json.size() && json[0] == ':', "Invalid json, missing ':' after key");
	json = json.substr(1);
	return key;
}
constexpr std::optional<double> parse_remove_json_double(std::string_view &json) {
	skip_whitespace(json);
	JSON_ASSERT(json.size(), "No number here");
	char* end = (char*)json.end();
	double d = std::strtod(json.data(), &end);
	json = json.substr(end - json.data());
	JSON_ASSERT(json.size(), "Missing cahracter after number");
	return d;
}
constexpr std::optional<bool> parse_remove_json_bool(std::string_view &json) {
	skip_whitespace(json);
	JSON_ASSERT(json.size(), "No bool here");
	size_t e = json.find_first_not_of("truefals");
	std::optional<bool> r;
	std::string_view bol = json.substr(0, e);
	if (bol == "true")
		r = true;
	else if (bol == "false")
		r = false;
	JSON_ASSERT(r.has_value(), "Failed to parse bool");
	JSON_ASSERT(json.size(), "Missing cahracter after bool");
	json = json.substr(e);
	return r;
}
template<typename T>
constexpr bool parse_remove_json_double_array(std::string_view &json, T &a) {
	skip_whitespace(json);
	JSON_ASSERT(json.size() && json[0] == '[', "Missing array start character");
	json = json.substr(1);
	for (size_t i = 0; i < a.size(); ++i) {
		auto v = parse_remove_json_double(json);
		JSON_ASSERT(v, "Failed to parse array double");
		*(a.data() + i) = v.value();
		if (json.size() && json[0] == ']')
			break;
		JSON_ASSERT(json.size() && json[0] == ',', "Array missing comma");
		json = json.substr(1);
	}
	JSON_ASSERT(json.size() && json[0] == ']', "Expected ']' at the end of array");
	json = json.substr(1);
	skip_whitespace(json);
	return true;
}
