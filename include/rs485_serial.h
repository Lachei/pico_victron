#pragma once

#include <hardware/uart.h>
#include <hardware/gpio.h>
#include <functional>
#include "pico/cyw43_arch.h"

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
		gpio_init(info.en_pin);
		gpio_set_dir(info.en_pin, GPIO_OUT);
		gpio_put(info.en_pin, 0); // by default enable receive
		uart_set_fifo_enabled(info.uart, true);
		uart_set_format(info.uart, info.data_bits, info.stop_bits, info.parity);
		uart_set_hw_flow(info.uart, false, false); // disable UART flow control CTS/RTS
		uart_set_fifo_enabled(info.uart, true); // enabling 32 byte fifo
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
			uart_putc_raw(info.uart, data[i]);
	}

	void enable_send() const {
		gpio_put(info.en_pin, 1);
	}

	void enable_receive() const {
		gpio_put(info.en_pin, 0);
	}

	void register_on_receive_callback(irq_handler_t cb) {
		int uart_irq = info.uart == uart0 ? UART0_IRQ : UART1_IRQ;
		irq_set_exclusive_handler(uart_irq, cb);
		irq_set_enabled(uart_irq, true);
		uart_set_irq_enables(uart0, true, false); // enable receive callback only after callback is set
	}

	void enable_receive_callback(bool enable) { 
		int uart_irq = info.uart == uart0 ? UART0_IRQ : UART1_IRQ;
		irq_set_enabled(uart_irq, enable);
	}
};

