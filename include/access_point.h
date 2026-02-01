#pragma once

#include <string_view>
#include "pico/cyw43_arch.h"
extern "C" {
#include "dhcpserver/dhcpserver.h"
#include "dnsserver/dnsserver.h"
}

/**
 * @brief Struct to hold all access point variables and to easily setup and teardown the access point
 * as well as handle incoming connections
 */
struct access_point {
	static access_point& Default() {
		static access_point ap{.name = "victron-steuerung", .password="12345678"};
		return ap;
	}
	// variables meant for public access
	std::string_view name;
	std::string_view password;
	bool active{false};

	// variables meant for private access (however also accessible from outside)
	dhcp_server_t _dhcp_server{};
	dns_server_t _dns_server{};
	ip4_addr_t _ip{};		// ip address of this device
	ip4_addr_t _mask{};		// ip mask

	void init() {
		if (active)
			return;
    		cyw43_arch_enable_ap_mode(name.data(), password.data(), CYW43_AUTH_WPA2_AES_PSK);
		#if LWIP_IPV6
		#define IP(x) ((x).u_addr.ip4)
		#else
		#define IP(x) (x)
		#endif
		IP(_ip).addr = PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
		IP(_mask).addr = PP_HTONL(CYW43_DEFAULT_IP_MASK);
		dhcp_server_init(&_dhcp_server, &_ip, &_mask);
		dns_server_init(&_dns_server, &_ip);
		active = true;
	}

	void deinit() {
		if (!active)
			return;
		dns_server_deinit(&_dns_server);
		dhcp_server_deinit(&_dhcp_server);
		cyw43_arch_disable_ap_mode();
		active = false;
	}
};

