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
#include "modbus_server.h"

#define TEST_TASK_PRIORITY ( tskIDLE_PRIORITY + 1UL )

constexpr UBaseType_t STANDARD_TASK_PRIORITY = tskIDLE_PRIORITY + 1ul;
constexpr UBaseType_t CONTROL_TASK_PRIORITY = tskIDLE_PRIORITY + 10ul;
constexpr uint GPIO_POWER = 26;
constexpr uint GPIO_MIN_CAP = 27;
constexpr float MAX_POWER = 4000;

uint32_t time_ms() { return time_us_64() / 1000; }
uint32_t time_s() { return time_us_64() / 1000000; }

void usb_comm_task(void *) {
    LogInfo("Usb communication task");
    crypto_storage::Default();

    for (;;) {
	handle_usb_command();
    }
}

static constexpr std::array<uint16_t, 101> soc_to_voltage_lfp = {
    250, 270, 280, 290, 300, 303, 305, 307, 309, 310, // 0% - 9%
    312, 314, 315, 317, 318, 319, 320, 321, 321, 322, // 10% - 19%
    322, 323, 323, 324, 324, 325, 325, 326, 326, 326, // 20% - 29%
    327, 327, 327, 327, 328, 328, 328, 328, 328, 329, // 30% - 39%
    329, 329, 329, 329, 329, 329, 330, 330, 330, 330, // 40% - 49%
    330, 330, 330, 330, 330, 331, 331, 331, 331, 331, // 50% - 59%
    331, 331, 332, 332, 332, 332, 332, 332, 332, 333, // 60% - 69%
    333, 333, 333, 333, 333, 333, 333, 334, 334, 334, // 70% - 79%
    334, 334, 334, 334, 334, 335, 335, 335, 336, 336, // 80% - 89%
    337, 338, 340, 342, 345, 348, 352, 355, 358, 360, 365 // 90% - 100%
};
static constexpr uint16_t soc_diff{soc_to_voltage_lfp.back() - soc_to_voltage_lfp.front()};
float soc_to_v(int soc, float min_v = settings::Default().bat_min_v, float max_v = settings::Default().bat_max_v) {
    uint16_t soc_v = soc_to_voltage_lfp[std::clamp(soc, 0, 101)];
    return (soc_v - soc_to_voltage_lfp[0]) / soc_diff * (max_v - min_v) + min_v; // rescale to soc to minv/maxv
}
float v_to_soc(float v, float min_v = settings::Default().bat_min_v, float max_v = settings::Default().bat_max_v) {
    float v_adopted = (v - min_v) / (max_v - min_v) * soc_diff + soc_to_voltage_lfp.front();
    int start = 0, end = soc_to_voltage_lfp.size();
    while (end - start > 1) {
        int middle_idx = std::min<int>((start + end) / 2, soc_to_voltage_lfp.size() - 1);
        uint16 middle = soc_to_voltage_lfp[middle_idx];
        if (v_adopted < middle)
            end = middle_idx;
        else
            start = middle_idx;
    }
    return start; // idx is soc value
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

// this task analyzes received frames and stores the returned information in the vebus structure
void vebus_comm_task(void *) {
    LogInfo("Starting VEBus comm monitor task");

    // note that all readout and setting of vebus information is done in webserver.h
    for (;;) {
        watchdog_update(); 
        VEBus::Default().Maintain();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

template<typename T>
T to_scaled(float value, sunssf scale_factor) {
    return static_cast<T>(value * std::pow(10, -scale_factor));
}
float to_unscaled(float value, sunssf scale_factor) {
    return value * std::pow(10, scale_factor);
}
// reads out settings from adc
float read_pot(uint gpio) {
    constexpr float SCALE{1. / 4096};
    adc_select_input(gpio - ADC_BASE_PIN);
    uint16_t adc = adc_read();
    return std::clamp(adc * SCALE * 1.5f - 0.25f, .0f, 1.f);
}
// Sunspec storage control (model 124, same as fronius gen24): the battery power (positive discharging) is
// kept inside [-InWRte, OutWRte] * WChaMax, where InWRte is only active with StorCtl_Mod bit0 and OutWRte with bit1.
// Negative rates force the other direction (eg. InWRte = -50% forces discharging with at least 50%).
// The adapter has no own optimization, so inside the allowed window it stays as close to 0W as possible.
// If no write happened for InOutWRte_RvrtTms seconds the limits are reverted to their defaults.
float sunspec_storage_power_w(modbus_register &r) {
    using regs = sunspec_registers;
    sunssf rate_sf = r.read(&regs::InOutWRte_SF);
    uint16_t revert_s = r.read(&regs::InOutWRte_RvrtTms);
    uint16_t mode = r.read(&regs::StorCtl_Mod);
    if (mode && revert_s && time_ms() - modbus_server::Default().last_write_ms >= revert_s * 1000u) {
        LogInfo("Storage control timed out, reverting");
        mode = 0;
        r.write(mode, &regs::StorCtl_Mod);
        r.write(to_scaled<int16>(100, rate_sf), &regs::InWRte);
        r.write(to_scaled<int16>(100, rate_sf), &regs::OutWRte);
    }
    float ref_w = to_unscaled(r.read(&regs::WChaMax), r.read(&regs::WChaMax_SF));
    float min_w = -ref_w; // most charging allowed
    float max_w = ref_w;  // most discharging allowed
    if (mode & STOR_CTL_LIMIT_CHARGE)
        min_w = -to_unscaled(r.read(&regs::InWRte), rate_sf) / 100 * ref_w;
    if (mode & STOR_CTL_LIMIT_DISCHARGE)
        max_w = to_unscaled(r.read(&regs::OutWRte), rate_sf) / 100 * ref_w;
    // all charging comes from the ac side, which is only allowed when charging from grid is enabled
    if (r.read(&regs::ChaGriSet) == CHA_GRI_SET_PV) {
        min_w = std::max(min_w, .0f);
        max_w = std::max(max_w, .0f); // a forced charge is not possible either
    }
    if (min_w > max_w) // contradicting limits, take the middle
        return (min_w + max_w) / 2;
    return std::clamp(.0f, min_w, max_w);
}

// positive goal_w means discharging, negative means charging
float get_remaining_w(float goal_w, float cur_v, float min_v, float max_v) {
    // 0.1V is the threshold where still max power is available. Afterwards linearly scales down
    if (goal_w > 0) // discharging
        return std::clamp(goal_w, .0f, std::max(cur_v - min_v, .0f) * 10 * MAX_POWER);
    else // charging
        return std::clamp(goal_w, std::min(cur_v - max_v, .0f) * 10 * MAX_POWER, .0f);
}
void victron_control_task(void *) {
    // register this control task to be signaled upon receiving a modbus query
    modbus_server::Default().external_ctrl_dependent_tasks.push(xTaskGetCurrentTaskHandle());

    constexpr float STABLE_V_A{.9};
    adc_init();
    adc_gpio_init(GPIO_POWER);
    adc_gpio_init(GPIO_MIN_CAP);
    settings &sets = settings::Default();
    modbus_register& mb_register = modbus_server::Default().registers;
    sets.external_w = 0;
    // the rates of the storage control are relative to WChaMax, so it has to report the real max power
    mb_register.write(to_scaled<uint16>(MAX_POWER, mb_register.read(&sunspec_registers::WChaMax_SF)), &sunspec_registers::WChaMax);

    uint32_t last_iter_ms = time_ms();
    float stable_v{};
    uint32_t last_set_ms{};
    float last_power{}; // negative for charging the battery, positive for discharging
    SwitchState last_mode{SwitchState::Sleep};


    for (;;) {
        uint32_t cur_ms = time_ms();
        float delta_ms = cur_ms - last_iter_ms;
        last_iter_ms = cur_ms;

        if (settings::changed)
		persistent_storage_t::Default().write(sets, &persistent_storage_layout::sets);
        settings::changed = false;

        sets.external_w = sunspec_storage_power_w(mb_register);

        // feed power to battery
        sets.local_w = read_pot(GPIO_POWER) * MAX_POWER;
        sets.local_min_soc = read_pot(GPIO_MIN_CAP) * 100;

        float cur_power{}; // negative for charging the battery, positive for discharging
        SwitchState cur_mode{SwitchState::Sleep};
        float cur_bat_v = VEBus::Default().GetDcInfo().Voltage;
        float external_w_avail = MAX_POWER;
        if (stable_v < 10)
            stable_v = cur_bat_v;
        else
            stable_v = STABLE_V_A * stable_v + (1 - STABLE_V_A) * cur_bat_v;
        float bat_soc_avail = v_to_soc(stable_v);
        float cur_bat_a = VEBus::Default().GetMultiPlusStatus().DcCurrentA;
        if (cur_bat_a > 2) { // give some epsilon to avoid strange behaviour around  0
            uint32_t cur_s = time_s();
            if (measurements::Default().energy_values.empty() || 
                cur_s - measurements::Default().last_load_time > 20) {
                measurements::Default().energy_values.push(0);
            }
            measurements::Default().last_load_time = cur_s;
            *measurements::Default().energy_values.back() += cur_bat_a * delta_ms / 1000 / 3600; // convert to Ah
        }
        if (sets.web_override) {
            cur_mode = from_web_state(sets.mode);
            float max_v, min_v;
            switch(cur_mode) {
                case SwitchState::Sleep: break;
                    external_w_avail = 0;
                    bat_soc_avail = 0;
                case SwitchState::ChargerOnly:
                    external_w_avail = 0;
                    bat_soc_avail = 0;
                    max_v = sets.min_max_type == MIN_MAX_TYPE_SOC ? soc_to_v(sets.max_soc): sets.max_v;
                    cur_power = get_remaining_w(-sets.max_w, stable_v, sets.bat_min_v, max_v);
                    break;
                case SwitchState::InverterOnly:
                    external_w_avail = 0;
                    bat_soc_avail = 0;
                    min_v = sets.min_max_type == MIN_MAX_TYPE_SOC ? soc_to_v(sets.min_soc): sets.min_v;
                    cur_power = get_remaining_w(-sets.min_w, stable_v, min_v, sets.bat_max_v);
                    break;
                case SwitchState::ChargerInverter:
                    cur_power = get_remaining_w(sets.external_w, stable_v, sets.bat_min_v, sets.bat_max_v);
                    break;
            }
        }
        if (!sets.web_override) {
            float target_min_v = soc_to_v(sets.local_min_soc);
            bat_soc_avail = (bat_soc_avail - sets.local_min_soc) / (1 - sets.local_min_soc / 100);
            cur_mode = SwitchState::ChargerInverter;
            if (stable_v < target_min_v) {
                external_w_avail = 0;
                cur_power = get_remaining_w(-sets.local_w, stable_v, target_min_v, sets.bat_max_v);
            } else {
                cur_power = get_remaining_w(sets.external_w, stable_v, target_min_v, sets.bat_max_v);
            }
        }

        if (last_mode != cur_mode || last_power != cur_power || (cur_ms - last_set_ms) >= 2000) {
            LogInfo("Switch mode to {:x}", (int)cur_mode);
            VEBus::Default().SetSwitch(cur_mode);
            LogInfo("Set power to {}", cur_power);
            VEBus::Default().SetPower(i16(cur_power));
            last_set_ms = cur_ms;
        }
        mb_register.write(to_scaled<uint16>(external_w_avail, mb_register.read(&sunspec_registers::WMax_SF)), 
                          &sunspec_registers::WMax);
        mb_register.write(to_scaled<uint16>(bat_soc_avail, mb_register.read(&sunspec_registers::ChaState_SF)), 
                          &sunspec_registers::ChaState);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)); // wait for 2 seconds but allow preemtive awake (happens when power is written via modbus)
    }
}

// callback which is called from modbus_server before the response is created
// register can also be queried for the gotten request if needed, see register.lc.fc for reqest function code
void update_modbus_values(modbus_register& mb_register) {
    using regs = sunspec_registers;
    MultiPlusStatus multi_status = VEBus::Default().GetMultiPlusStatus();
    DcInfo dc_info = VEBus::Default().GetDcInfo();
    AcInfo ac_info = VEBus::Default().GetAcInfo(PhaseInfo::S_L1);

    mb_register.write(ac_info.MainCurrent                       , &regs::A);
    mb_register.write(ac_info.MainVoltage                       , &regs::PhVphA);
    mb_register.write(ac_info.MainCurrent * ac_info.MainVoltage , &regs::W);
    mb_register.write(50.f                                      , &regs::Hz);
    mb_register.write(multi_status.DcCurrentA                   , &regs::DCA);
    mb_register.write(dc_info.Voltage                           , &regs::DCV);
    mb_register.write(multi_status.DcCurrentA * dc_info.Voltage , &regs::DCW);
    mb_register.write(multi_status.Temp                         , &regs::TmpCab);
    // TODO: maybe write mppt model registers
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
    persistent_storage_t::Default().read(&persistent_storage_layout::sets, settings::Default());
    settings::Default().sanitize(); // will make sure that no garbage is loaded from storage
    Webserver().start();
    VEBus::Default().Setup(); // creates a separate thread for rs485 communication
    modbus_server::Default().pre_response_function = update_modbus_values;
    modbus_server::Default().start();
    LogInfo("Initialization done");
    std::cout << "Initialization done, get all further info via the commands shown in 'help'\n";
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);

    xTaskCreate(usb_comm_task, "UsbComm", 512, NULL, 1, NULL); // usb task also has to be started only after cyw43 init as some wifi functions are available
    xTaskCreate(wifi_search_task, "UpdateWifi", 512, NULL, 1, NULL);
    xTaskCreate(vebus_comm_task, "VEBusComm", 2048, NULL, 1, NULL);
    xTaskCreate(victron_control_task, "VictronControl", 2048, NULL, 1, NULL);
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
