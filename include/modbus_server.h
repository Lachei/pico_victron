#pragma once

#include <functional>
#include <modbus-register.h>
#include <static_types.h>

#include <FreeRTOS.h>
#include <task.h>

#include "../modbus_layouts/inverter_sunspec.h"

#include "lwip/tcp.h"

using modbus_register = libmodbus_static::modbus_register<inverter_layout>;

struct modbus_server {

	static modbus_server& Default() {
		static modbus_register regs{/*server address*/1};
		static modbus_server msr{regs};
		return msr;
	}

	modbus_register &registers;

	uint16_t port{502}; // default modbus tcp port is 502
	int tcp_poll_time_s{30};
	struct tcp_pcb *tcp_pcb{};
	static_vector<TaskHandle_t, 4> external_ctrl_dependent_tasks{}; // tasks in here will be unlocked
	std::function<void(modbus_register&)> pre_response_function{};
	

	/** @brief Starts the lwip tcp server on specified port 
	 * Also pulls updated values from ve-bus automatically and updates its registers
	 * Further also sets externally set values
	 **/
	err_t start();
	err_t stop();
};

