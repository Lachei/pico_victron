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

float soc_to_v(float soc, float min_v = settings::Default().bat_min_v, float max_v = settings::Default().bat_max_v) {
    return (soc / 100) * (max_v - min_v) + min_v;
}

void wifi_search_task(void *) {
    LogInfo("Wifi task started");
    if (wifi_storage::Default().ssid_wifi.empty()) // onyl start the access point by default if no normal wifi connection is set
        access_point::Default().init();

    constexpr uint32_t ap_timeout = 10;
    uint32_t cur_time = time_s();
    uint32_t last_conn = cur_time;

    for (;;) {
        cur_time = time_s();
        uint32_t dt = cur_time - last_conn;
        wifi_storage::Default().update_hostname();
        wifi_storage::Default().update_wifi_connection();
        if (wifi_storage::Default().wifi_connected)
            last_conn = cur_time;
        if (dt % 30 == 5) // every 30 seconds enable reconnect try
            wifi_storage::Default().wifi_changed = true;
        if (dt > ap_timeout) {
            access_point::Default().init();
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, cur_time & 1);
        } else {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, wifi_storage::Default().wifi_connected);
        }
        wifi_storage::Default().update_scanned();
        if (wifi_storage::Default().wifi_connected)
            ntp_client::Default().update_time();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
void vebus_comm_task(void *) {
    LogInfo("Starting VEBus comm monitor task");

    // note that all readout and setting of vebus information is done in webserver.h
    for (;;) {
        watchdog_update(); 
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
    settings &sets = settings::Default();
    sets.external_w = 0;

    for (;;) {
        if (settings::changed)
		persistent_storage_t::Default().write(sets, &persistent_storage_layout::sets);
        settings::changed = false;

        sets.local_w = read_pot(GPIO_POWER) * 6000;
        sets.local_min_v = soc_to_v(read_pot(GPIO_MIN_CAP) * 100);

        float cur_power{}; // negative for charging the battery, positive for discharging
        SwitchState cur_mode{SwitchState::Sleep};
        float cur_bat_v = VEBus::Default().GetDcInfo().Voltage;
        if (sets.web_override) {
            cur_mode = from_web_state(sets.mode);
            float max_v, min_v;
            switch(cur_mode) {
                case SwitchState::Sleep: break;
                case SwitchState::ChargerOnly:
                    max_v = sets.min_max_type == MIN_MAX_TYPE_SOC ? soc_to_v(sets.max_soc): sets.max_v;
                    cur_power = cur_bat_v < max_v ? -sets.max_w: 0;
                    break;
                case SwitchState::InverterOnly:
                    min_v = sets.min_max_type == MIN_MAX_TYPE_SOC ? soc_to_v(sets.min_soc): sets.min_v;
                    cur_power = cur_bat_v > min_v ? -sets.min_w: 0;
                    break;
                case SwitchState::ChargerInverter:
                    cur_power = sets.external_w;
                    break;
            }
        }
        if (!sets.web_override) {
            if (cur_bat_v < sets.local_min_v) {
                cur_power = -sets.local_w;
                cur_mode = SwitchState::ChargerInverter;
            } else {
                cur_power = sets.external_w;
                cur_mode = SwitchState::ChargerInverter;
            }
        }

        LogInfo("Switch mode to {:x}", (int)cur_mode);
        VEBus::Default().SetSwitch(cur_mode);
        LogInfo("Set power to {}", cur_power);
        VEBus::Default().SetPower(i16(cur_power));
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
    wifi_storage::Default().update_hostname();
    wifi_storage::Default().update_scanned();
    Webserver().start();
    LogInfo("Ready, running http at {}", ip4addr_ntoa(netif_ip4_addr(netif_list)));
    VEBus::Default().Setup(); // creates a separate thread
    persistent_storage_t::Default().read(&persistent_storage_layout::sets, settings::Default());
    settings::Default().sanitize(); // will make sure that no garbage is loaded from storage
    LogInfo("Initialization done");
    std::cout << "Initialization done, get all further info via the commands shown in 'help'\n";
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    xTaskCreate(usb_comm_task, "UsbComm", 512, NULL, 1, NULL); // usb task also has to be started only after cyw43 init as some wifi functions are available
    xTaskCreate(wifi_search_task, "UpdateWifi", 512, NULL, 1, NULL);
    xTaskCreate(vebus_comm_task, "VEBusComm", 2048, NULL, 8, NULL);
    xTaskCreate(victron_control_task, "VictronControl", 2048, NULL, 8, NULL);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    vTaskDelete(NULL); // remove this task for efficiency reasions
}

int main( void )
{
    stdio_init_all();

    LogInfo("Starting FreeRTOS on all cores.");
    std::cout << "Starting FreeRTOS on all cores\n";

    if (watchdog_enable_caused_reboot())
        LogError("Rebooted by Watchdog!");
    watchdog_start_tick(15); // set tick divider to 150 Mhz
    watchdog_enable(5000/*ms*/, /*Stop on debug mode off*/0);

    TaskHandle_t task_startup;
    xTaskCreate(startup_task, "StartupThread", 512, NULL, 1, &task_startup);

    vTaskStartScheduler();
    return 0;
}
