#pragma once

#include <iostream>

#include "static_types.h"


struct measurements {
	static_ring_buffer<float, 32> energy_values{};
	uint32_t last_load_time{};

	static measurements& Default() {
		static measurements m{};
		return m;
	}
};

/** @brief prints formatted for monospace output, eg. usb */
std::ostream& operator<<(std::ostream &os, const measurements &m) {
	os << "last_load_tim: " << m.last_load_time << '\n';
	os << "energy_values: [";
	for (const float &value: m.energy_values)
		os << (&value == &*m.energy_values.begin()? ' ': ',') << value;
	os << "]\n";

	return os;
}

