#pragma once

#include <iostream>

#include "static_types.h"
#include "json_util.h"

#define ASSERT_TRUE(e) if (!(e)) return {};
std::string_view pb(bool b);

constexpr int MIN_MAX_TYPE_SOC = 0;
constexpr int MIN_MAX_TYPE_V = 0;

struct settings {
	bool web_override{};
	int mode{}; // corresponds to VEBusDefinition::SwitchState, convert with from/to_web_state
	int min_max_type{}; // 0 -> SOC, 1 -> V
	float min_soc{5};
	float max_soc{95};
	float min_v{46};
	float max_v{55};
	float min_w{-4000};
	float max_w{4000};
	float local_w{4000};
	float local_min_v{52};
	float external_w{}; // used for external power setting, positive for discharging batter, negative for charging
	float bat_min_v{46};
	float bat_max_v{56};

	static settings& Default() {
		static settings s{};
		return s;
	}
	/** @brief writes the settings struct as json to the static strig s */
	template<int N>
	constexpr void dump_to_json(static_string<N> &s) const {
		s.append_formatted(R"({{"override":{},"mode":{},"mmtype":{},"mins":{},"maxs":{},"minv":{},"maxv":{},"minw":{},"maxw":{},"localw":{},"localminv":{},"batminv":{},"batmaxv":{}}})", 
		     pb(web_override), mode, min_max_type, min_soc, max_soc, min_v, max_v, min_w, max_w, local_w, local_min_v, bat_min_v, bat_max_v);
	}
	constexpr bool parse_from_json(std::string_view json) {
		ASSERT_TRUE(parse_remove_json_obj_start(json));
		for(int i = 0; i < 56 && json.size(); ++i) {
			auto key = parse_remove_json_key(json);
			ASSERT_TRUE(key);
			if (key == "override") {
				auto override = parse_remove_json_bool(json);
				ASSERT_TRUE(override);
				settings::Default().web_override = override.value();
			} else if (key == "mode") {
				auto mode = parse_remove_json_double(json);
				ASSERT_TRUE(mode);
				settings::Default().mode = static_cast<int>(mode.value());
			} else if (key == "mmtype") {
				auto mmtype = parse_remove_json_double(json);
				ASSERT_TRUE(mmtype);
				settings::Default().min_max_type = static_cast<int>(mmtype.value());
			} else if (key == "mins") {
				auto mins = parse_remove_json_double(json);
				ASSERT_TRUE(mins);
				settings::Default().min_soc = mins.value();
			} else if (key == "maxs") {
				auto maxs = parse_remove_json_double(json);
				ASSERT_TRUE(maxs);
				settings::Default().max_soc = maxs.value();
			} else if (key == "minv") {
				auto minv = parse_remove_json_double(json);
				ASSERT_TRUE(minv);
				settings::Default().min_v = minv.value();
			} else if (key == "maxv") {
				auto maxv = parse_remove_json_double(json);
				ASSERT_TRUE(maxv);
				settings::Default().max_v = maxv.value();
			} else if (key == "minw") {
				auto minw = parse_remove_json_double(json);
				ASSERT_TRUE(minw);
				settings::Default().min_w = minw.value();
			} else if (key == "maxw") {
				auto maxw = parse_remove_json_double(json);
				ASSERT_TRUE(maxw);
				settings::Default().max_w = maxw.value();
			} else if (key == "batminv") {
				auto minv = parse_remove_json_double(json);
				ASSERT_TRUE(minv);
				settings::Default().bat_min_v = minv.value();
			} else if (key == "batmaxv") {
				auto maxv = parse_remove_json_double(json);
				ASSERT_TRUE(maxv);
				settings::Default().bat_max_v = maxv.value();
			} else {
				LogError("Invalid key {}", key.value());
				return false;
			}
			if (try_parse_remove_json_obj_end(json))
				break;
			ASSERT_TRUE(try_parse_remove_json_sep(json));
		}
		return true;
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
	os << "local_min_v  : " << s.local_min_v << std::endl;
	os << "bat_min_v    : " << s.bat_min_v << std::endl;
	os << "bat_max_v    : " << s.bat_max_v << std::endl;
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
	else if (key == "local_min_v") 
		is >> s.local_min_v;
	else if (key == "bat_min_v") 
		is >> s.bat_min_v;
	else if (key == "bat_max_v") 
		is >> s.bat_max_v;
	else
		is.fail();
	return is;
}

