/*
 Name:		VEBus.h
 Created:	07.02.2024 20:07:36
 Author:	nriedle
 Adopted:       08.01.2025 josefstumpfegger@outlook.de
 License:	GPL v3: https://github.com/GitNik1/VEBus/blob/master/LICENSE
*/

#pragma once

#define UART_MODE_RS485

#include "ve_bus_definition.h"

#include <functional>
#include <variant>
#include <FreeRTOS.h>
#include <semphr.h>

using namespace VEBusDefinition;
struct VEBus
{
    static VEBus& Default() {
        static Serial serial{SerialInfos};
        static VEBus ve_bus{serial};
        return ve_bus;
    }


    struct ResponseData
    {
        uint8_t id;
        uint8_t command;
        uint8_t address;
        std::variant<u32, i32, f32> value;
    };

    enum RequestError
    {
        Success,
        FifoFull,
        OutsideLowerRange,
        OutsideUpperRange,
        ConvertError
    };

    struct RequestResult
    {
        uint8_t id;
        RequestError error;
    };

    friend void communication_task(void* handler_args);

    std::function<void(ResponseData&)> response_cb;
    std::function<void(VEBusBuffer&)> receive_cb;

    VEBus(Serial& serial);
    ~VEBus();

    void Setup(bool autostart = true);
    void Maintain();

    void StartCommunication();
    void StopCommunication();

    //*Be careful when repeatedly writing EEPROM (loop)
    //*EEPROM writes are limited
    //*Returns 0 if failed
    using val_var = std::variant<u16, i16, f32>;
    RequestResult WriteViaID(RamVariables variable, val_var value, bool eeprom = false);
    RequestResult WriteViaID(Settings setting, val_var value, bool eeprom = false);

    // Charge battery with negative power values, discharge with positive numbers
    RequestResult SetPower(i16 power_w);

    //*Read EEPROM saved Value
    //*Returns 0 if failed
    uint8_t Read(RamVariables variables);
    uint8_t Read(Settings setting);

    uint8_t ReadInfo(RamVariables variable);
    uint8_t ReadInfo(Settings setting);

    void SetSwitch(SwitchState state);
 
    const RAMVarInfo& GetRamVarInfo(RamVariables variable);
    const SettingInfo& GetSettingInfo(Settings setting);

    bool NewMasterMultiLedAvailable();
    MasterMultiLed GetMasterMultiLed();

    bool NewMultiPlusStatusAvailable();
    MultiPlusStatus GetMultiPlusStatus();

    bool NewDcInfoAvailable();
    DcInfo GetDcInfo();

    uint8_t NewAcInfoAvailable();
    AcInfo GetAcInfo(uint8_t type);

    //Get VE.BUS Version
    uint8_t ReadSoftwareVersion();
    uint8_t CommandReadDeviceState();

    struct Data
    {
        bool responseExpected;
        bool IsSent = false;
        bool IsLogged = false;
        uint8_t id = 0;
        uint8_t command;
        uint8_t address;
        uint8_t expectedResponseCode = 0;
        uint32_t sentTimeMs;
        uint32_t resendCount = 0;
        VEBusBuffer requestData{};
        VEBusBuffer responseData{};
    };

    Serial& _serial;
    SemaphoreHandle_t _semaphoreDataFifo;
    SemaphoreHandle_t _semaphoreStatus;
    SemaphoreHandle_t _semaphoreReceiveData;
    uint8_t _id;
    static_vector<Data, VEBUS_FIFO_SIZE> _dataFifo;
    //Runs on core 0. not thread save.
    VEBusBuffer _receiveBuffer;
    static_vector<VEBusBuffer, VEBUS_MAX_RECEIVE_BUFFER> _receiveBufferList;
    SettingInfos _settingInfoList = DefaultSettingInfos;
    RAMVarInfos _ramVarInfoList = DefaultRamVarInfos;
    static_vector<AcInfo, PHASES_COUNT> _acInfo;
    DcInfo _dcInfo;

    MasterMultiLed _masterMultiLed;
    volatile bool _masterMultiLedNewData = false;
    volatile bool _masterMultiLedLogged = false;

    MultiPlusStatus _multiPlusStatus;
    volatile bool _multiPlusStatusNewData = false;
    volatile bool _multiPlusStatusLogged = false;

    bool _communitationIsRunning = false;
    volatile bool _communitationIsResumed = false;

    void addOrUpdateFifo(const Data &data, bool updateIfExist = true);


    bool getNextFreeId_1(uint8_t id);

    ReceivedMessageType decodeVEbusFrame(VEBusBuffer &buffer);
    void decodeChargerInverterCondition(VEBusBuffer &buffer); //0x80
    void decodeBatteryCondition(VEBusBuffer &buffer); //0x70
    void decodeMasterMultiLed(VEBusBuffer &buffer); //0x41
    void decodeInfoFrame(VEBusBuffer &buffer); // 0x20

    void saveSettingInfoData(const Data& data);
    void saveRamVarInfoData(const Data& data);
    void commandHandling();

    void sendData(VEBus::Data& data, uint8_t frameNr);
    void saveResponseData(const Data &data);
    void checkResponseMessage();
    void checkResponseTimeout();
};

