#pragma once

#include <hardware/uart.h>
#include <hardware/gpio.h>

struct rs485_serial {
	struct rs485_info {
		uart_inst_t *uart{uart0};
		uint baudrate{256000};
		int tx_pin{0};
		int rx_pin{1};
		int en_pin{2};
		int data_bits{8};
		int stop_bits{1};
		uart_parity_t parity{UART_PARITY_NONE};
		static rs485_info Default() {return {};}
	};
	rs485_info info;
	rs485_serial(const rs485_info &info = rs485_info::Default()): info{info} {
		this->info.baudrate = uart_init(info.uart, info.baudrate);
		gpio_set_function(info.tx_pin, GPIO_FUNC_UART);
		gpio_set_function(info.rx_pin, GPIO_FUNC_UART);
		gpio_set_dir(info.en_pin, GPIO_OUT);
		gpio_put(info.en_pin, 0); // by default enable receive
		uart_set_fifo_enabled(info.uart, true);
		uart_set_format(info.uart, info.data_bits, info.stop_bits, info.parity);
	}

	void tx_flush() const {
		uart_tx_wait_blocking(info.uart);
	}

	bool rx_available() const {
		return uart_is_readable(info.uart);
	}

	char getc() const {
		return uart_getc(info.uart);
	}

	void write(const uint8_t *data, size_t size) {
		for (size_t i = 0; i < size; ++i)
			uart_putc(info.uart, data[i]);
	}

	void enable_send() const {
		gpio_put(info.en_pin, 1);
	}

	void enable_receive() const {
		gpio_put(info.en_pin, 0);
	}
};

