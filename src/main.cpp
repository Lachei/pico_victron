/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"

#include "lwip/ip4_addr.h"
#include "lwip/apps/mdns.h"

#include "FreeRTOS.h"
#include "task.h"

#include "log_storage.h"
#include "access_point.h"
#include "wifi_storage.h"
#include "webserver.h"
#include "usb_interface.h"
#include "settings.h"
#include "measurements.h"
#include "crypto_storage.h"
#include "ntp_client.h"
#include "ve_bus.h"

#define TEST_TASK_PRIORITY ( tskIDLE_PRIORITY + 1UL )

constexpr UBaseType_t STANDARD_TASK_PRIORITY = tskIDLE_PRIORITY + 1ul;
constexpr UBaseType_t CONTROL_TASK_PRIORITY = tskIDLE_PRIORITY + 10ul;
constexpr uint GPIO_POWER = 26;
constexpr uint GPIO_MIN_CAP = 27;

uint32_t time_s() { return time_us_64() / 1000000; }

void usb_comm_task(void *) {
    LogInfo("Usb communication task");
    crypto_storage::Default();

    for (;;) {
	handle_usb_command();
    }
}

void wifi_search_task(void *) {
    LogInfo("Wifi task started");
    if (wifi_storage::Default().ssid_wifi.empty()) // onyl start the access point by default if no normal wifi connection is set
        access_point::Default().init();

    wifi_storage::Default().update_hostname();

    constexpr uint32_t ap_timeout = 10;
    uint32_t cur_time = time_s();
    uint32_t last_conn = cur_time;

    for (;;) {
        LogInfo("Wifi update loop");
        cur_time = time_s();
        uint32_t dt = cur_time - last_conn;
        wifi_storage::Default().update_wifi_connection();
        if (wifi_storage::Default().wifi_connected)
            last_conn = cur_time;
        if (dt > ap_timeout) {
            access_point::Default().init();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cur_time & 1);
        } else {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, wifi_storage::Default().wifi_connected);
        }
        wifi_storage::Default().update_hostname();
        wifi_storage::Default().update_scanned();
        if (wifi_storage::Default().wifi_connected)
            ntp_client::Default().update_time();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vebus_infos_task(void *) {
    LogInfo("Starting to monitor ve bus");
    // note that all readout and setting of vebus information is done in webserver.h
    for (;;) {
        watchdog_update(); // the ve_bus task is most important
        VEBus::Default().Maintain();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// reads out settings from adc
float read_pot(uint gpio) {
    adc_select_input(GPIO_POWER - ADC_BASE_PIN);
    uint16_t adc = adc_read();
    return adc / float(4096);
}
void victron_control_task(void *) {
    adc_init();
    adc_gpio_init(GPIO_POWER);
    adc_gpio_init(GPIO_MIN_CAP);
    float last_power = 0;

    for (;;) {
        settings::Default().local_w = read_pot(GPIO_POWER) * 6000;
        settings::Default().local_min_soc = read_pot(GPIO_MIN_CAP) * 100;
        if (!settings::Default().web_override && std::abs(settings::Default().local_w - last_power) > 100) {
            VEBus::Default().SetPower(i16(-settings::Default().local_w));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// task to initailize everything and only after initialization startin all other threads
// cyw43 init has to be done in freertos task because it utilizes freertos synchronization variables
void startup_task(void *) {
    LogInfo("Starting initialization");
    std::cout << "Starting initialization\n";
    if (cyw43_arch_init()) {
        for (;;) {
            vTaskDelay(1000);
            LogError("failed to initialize\n");
            std::cout << "failed to initialize arch (probably ram problem, increase ram size)\n";
        }
    }
    cyw43_arch_enable_sta_mode();
    Webserver().start();
    LogInfo("Ready, running http at {}", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    VEBus::Default().Setup();
    settings::Default();
    LogInfo("Initialization done");
    std::cout << "Initialization done, get all further info via the commands shown in 'help'\n";
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    xTaskCreate(usb_comm_task, "UsbComm", 512, NULL, 1, NULL); // usb task also has to be started only after cyw43 init as some wifi functions are available
    xTaskCreate(wifi_search_task, "UpdateWifiThread", 512, NULL, 1, NULL);
    xTaskCreate(victron_control_task, "VictronControlThread", 512, NULL, 1, NULL);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    vebus_infos_task({});
}

int main( void )
{
    stdio_init_all();

    LogInfo("Starting FreeRTOS on all cores.");
    std::cout << "Starting FreeRTOS on all cores\n";

    if (watchdog_enable_caused_reboot())
        LogError("Rebooted by Watchdog!");
    watchdog_enable(500000/*us*/, /*Stop on debug mode off*/0);

    TaskHandle_t task_startup;
    xTaskCreate(startup_task, "StartupThread", 512, NULL, 1, &task_startup);

    vTaskStartScheduler();
    return 0;
}
