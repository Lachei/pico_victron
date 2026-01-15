#pragma once

#include <iostream>

#include "static_types.h"

std::string_view pb(bool b);

struct settings {
	bool web_override{};
	int mode{}; // corresponds to VEBusDefinition::SwitchState, convert with from/to_web_state
	int min_max_type{}; // 0 -> SOC, 1 -> V
	float min_soc{};
	float max_soc{};
	float min_v{};
	float max_v{};
	float min_w{};
	float max_w{};
	float local_w{};
	float local_min_soc{};

	static settings& Default() {
		static settings s{};
		return s;
	}
	/** @brief writes the settings struct as json to the static strig s */
	template<int N>
	constexpr void dump_to_json(static_string<N> &s) const {
		s.append_formatted(R"({{"web_override":{},"mode":{},"min_max_type":{},"min_soc":{},"max_soc":{},"min_v":{},"max_v":{},"min_w":{},"max_w":{},"local_w":{},"local_min_soc":{}}})", 
		     pb(web_override), mode, min_max_type, min_soc, max_soc, min_v, max_v, min_w, max_w, local_w, local_min_soc);
	}
};

/** @brief prints formatted for monospace output, eg. usb */
std::ostream& operator<<(std::ostream &os, const settings &s) {
	os << "web_override : " << pb(s.web_override) << std::endl;
	os << "mode         : " << s.mode << std::endl;
	os << "min_max_type : " << s.min_max_type << std::endl;
	os << "min_soc      : " << s.min_soc << std::endl;
	os << "max_soc      : " << s.max_soc << std::endl;
	os << "min_v        : " << s.min_v << std::endl;
	os << "max_v        : " << s.max_v << std::endl;
	os << "min_w        : " << s.min_w << std::endl;
	os << "max_w        : " << s.max_w << std::endl;
	os << "local_w      : " << s.local_w << std::endl;
	os << "local_min_soc: " << s.local_min_soc << std::endl;
	return os;
}

/** @brief parses a single key, value pair from the istream */
std::istream& operator>>(std::istream &is, settings &s) {
	std::string key;
	is >> key;
	if (key == "web_override")
		is >> s.web_override;
	else if (key == "mode") 
		is >> s.mode;
	else if (key == "min_max_type") 
		is >> s.min_max_type;
	else if (key == "min_soc") 
		is >> s.min_soc;
	if (key == "max_soc") 
		is >> s.max_soc;
	else if (key == "min_v") 
		is >> s.min_v;
	else if (key == "max_v") 
		is >> s.max_v;
	else if (key == "min_w") 
		is >> s.min_w;
	else if (key == "max_w") 
		is >> s.max_w;
	else if (key == "local_w") 
		is >> s.local_w;
	else if (key == "local_min_soc") 
		is >> s.local_min_soc;
	else
		is.fail();
	return is;
}

