/*
 Name:		VEBus.cpp
 Created:	07.02.2024 20:07:36
 Author:	nriedle
 Adopted:       08.01.2025 josefstumpfegger@outlook.de
 License:	GPL v3: https://github.com/GitNik1/VEBus/blob/master/LICENSE
*/

#include "ve_bus.h"
#include "log_storage.h"

#define NEXT_FRAME_NR(x) (x + 1) & 0x7F
#define MK3_ID_0 0x98 //8F?
#define MK3_ID_1 0xF7
#define MP_ID_0 0x83
#define MP_ID_1 0x83
#define SYNC_BYTE 0x55
#define SYNC_FRAME 0xFD
#define DATA_FRAME 0xFE
#define END_OF_FRAME 0xFF
#define LOW_BATTERY 0x02

void commandHandling(VEBus &ve_bus);
void fill_command_buffer(VEBusBuffer &buffer, uint8_t id, uint8_t winmonCommand, StorageType storageType, uint8_t address, uint8_t lowByte, uint8_t highByte);
uint8_t prepareCommandReadMultiRAMVar(VEBusBuffer &buffer, uint8_t id, uint8_t* addresses, uint8_t addressSize);
void prepareCommandReadSetting(VEBusBuffer &buffer, uint8_t id, uint16_t address);
void prepareCommandWriteAddress(VEBusBuffer &buffer, uint8_t id, uint8_t winmonCommand, uint16_t address);
void prepareCommandReadInfo(VEBusBuffer& buffer, uint8_t id, uint8_t winmonCommand, uint16_t setting);
void prepareCommandSetSwitchState(VEBusBuffer& buffer, SwitchState switchState);
void prepareCommandReadSoftwareVersion(VEBusBuffer& buffer, uint8_t id, uint8_t winmonCommand);;
void prepareCommandSetGetDeviceState(VEBusBuffer& buffer, uint8_t id, CommandDeviceState command, uint8_t state = 0);
void appendChecksum(std::vector<uint8_t>& buffer);
uint16_t convertRamVarToRawValue(RamVariables variable, float value, const RAMVarInfos &ramVarInfoList);
float convertRamVarToValue(RamVariables variable, uint16_t rawValue, const RAMVarInfos &ramVarInfoList);
int16_t convertRamVarToRawValueSigned(RamVariables variable, float value, const RAMVarInfos &ramVarInfoList);
float convertRamVarToValueSigned(RamVariables variable, int16_t rawValue, const RAMVarInfos &ramVarInfoList);
uint16_t convertSettingToRawValue(Settings setting, float value, const SettingInfos &settingInfoList);
float convertSettingToValue(Settings setting, uint16_t rawValue, const SettingInfos &settingInfoList);

void communication_task(void* handler_args)
{
	VEBus *ve_bus = static_cast<VEBus*>(handler_args);

	while (true)
	{
		ve_bus->commandHandling();
		//vTaskDelay(1 / portTICK_RATE_MS);
		taskYIELD();
	}
}

VEBus::VEBus(Serial& serial) : _serial(serial)
{
	_semaphoreDataFifo = xSemaphoreCreateMutex();
	_semaphoreStatus = xSemaphoreCreateMutex();
	_semaphoreReceiveData = xSemaphoreCreateMutex();
}

VEBus::~VEBus()
{
}

void VEBus::Setup(bool autostart)
{
	if (autostart) StartCommunication();
	xTaskCreate(communication_task, "vebus_task", 4096, this, 1, NULL);
}

void VEBus::Maintain()
{
	checkResponseTimeout();
	checkResponseMessage();

	xSemaphoreTake(_semaphoreReceiveData, portMAX_DELAY);
	if (receive_cb)
		for (VEBusBuffer &d: _receiveBufferList)
			receive_cb(d);
	_receiveBufferList.clear();

	xSemaphoreGive(_semaphoreReceiveData);
}

void VEBus::StartCommunication()
{
	_communitationIsRunning = true;
	_communitationIsResumed = true;

}

void VEBus::StopCommunication()
{
	_communitationIsRunning = false;
}

VEBus::RequestResult VEBus::WriteViaID(RamVariables variable, val_var value, bool eeprom)
{
	if (!_ramVarInfoList[variable].available) 
		return {0 , RequestError::ConvertError };
	uint8_t lowByte{};
	uint8_t highByte{};

	if (u16 *val = std::get_if<u16>(&value)) {
		lowByte = (*val) & 0xff;
		highByte = (*val) >> 8;
	} else if (i16 *val = std::get_if<i16>(&value)) {
		lowByte = (*val) & 0xff;
		highByte = (*val) >> 8;
	} else if (f32 *val = std::get_if<f32>(&value)) {
		if (_ramVarInfoList[variable].Scale < 0)
		{
			i16 v = convertRamVarToRawValueSigned(variable, *val, _ramVarInfoList);
			lowByte = v & 0xff;
			highByte = v >> 8;
		} else {
			u16 v = convertRamVarToRawValue(variable, *val, _ramVarInfoList);
			lowByte = v & 0xff;
			highByte = v >> 8;
		}
	} else
		LogError("WTF");

	Data data;
	if (!getNextFreeId_1(data.id)) 
		return {0 , RequestError::FifoFull };
	data.responseExpected = true;
	data.command = WinmonCommand::WriteRAMVar;
	data.address = variable;
	data.expectedResponseCode = 0x87;
	StorageType storageType = (eeprom == false) ? StorageType::NoEeprom : StorageType::Eeprom;
	fill_command_buffer(data.requestData, data.id, data.command, storageType, variable, lowByte, highByte);
	addOrUpdateFifo(data);
	return { data.id , RequestError::Success };
}


VEBus::RequestResult VEBus::WriteViaID(Settings setting, val_var value, bool eeprom)
{
	uint8_t lowByte{};
	uint8_t highByte{};

	if (u16 *val = std::get_if<u16>(&value)) {
		lowByte = (*val) & 0xff;
		highByte = (*val) >> 8;
	} else if (i16 *val = std::get_if<i16>(&value)) {
		lowByte = (*val) & 0xff;
		highByte = (*val) >> 8;
	} else if (f32 *val = std::get_if<f32>(&value)) {
		uint16_t rawValue = convertSettingToRawValue(setting, *val, _settingInfoList);
		if (_settingInfoList[setting].Maximum < rawValue) return {0, RequestError::OutsideUpperRange};
		if (_settingInfoList[setting].Minimum > rawValue) return {0, RequestError::OutsideLowerRange};
	}
	Data data;
	if (!getNextFreeId_1(data.id)) 
		return {0 , RequestError::FifoFull };
	data.responseExpected = true;
	data.command = WinmonCommand::WriteSetting;
	data.address = setting;
	data.expectedResponseCode = 0x87;
	StorageType storageType = (eeprom == false) ? StorageType::NoEeprom : StorageType::Eeprom;
	fill_command_buffer(data.requestData, data.id, data.command, storageType, setting, lowByte, highByte);
	addOrUpdateFifo(data);
	return { data.id , RequestError::Success };
}

VEBus::RequestResult VEBus::SetPower(i16 power_w)
{
	uint8_t lowByte = power_w & 0xff;
	uint8_t highByte = power_w >> 8;
	Data data;
	if (!getNextFreeId_1(data.id)) 
		return {0 , RequestError::FifoFull };
	data.responseExpected = true;
	data.command = WinmonCommand::WriteRAMVar;
	data.address = 0x83;
	data.expectedResponseCode = 0x87;
	StorageType storageType =  StorageType::NoEeprom;
	fill_command_buffer(data.requestData, data.id, data.command, storageType, data.address, lowByte, highByte);
	addOrUpdateFifo(data);
	return { data.id , RequestError::Success };
}

uint8_t VEBus::Read(RamVariables variable)
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::ReadRAMVar;
	data.address = variable;
	data.expectedResponseCode = 0x85;
	uint8_t address[] = { variable };
	prepareCommandReadMultiRAMVar(data.requestData, data.id, address, 1);
	addOrUpdateFifo(data);
	return data.id;
}

// Command: 0x31 < Lo(Setting ID) > <Hi(Setting ID)> 
// Response : 0x86 / 91 < Lo(Value) > <Hi(Value)>  
// <Value> is an unsigned 16 - bit quantity.  
// 0x86 = SettingReadOK. 
// 0x91 = Setting not supported(in which case <Value> is not valid).
uint8_t VEBus::Read(Settings setting)
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::ReadSetting;
	data.address = setting;
	data.expectedResponseCode = 0x86;
	prepareCommandReadSetting(data.requestData, data.id, setting);
	addOrUpdateFifo(data);
	return data.id;
}

uint8_t VEBus::ReadInfo(RamVariables variable)
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::GetRAMVarInfo;
	data.address = variable;
	data.expectedResponseCode = 0x8E;
	prepareCommandReadInfo(data.requestData, data.id, data.command, variable);
	addOrUpdateFifo(data);
	return data.id;
}

uint8_t VEBus::ReadInfo(Settings setting)
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::GetSettingInfo;
	data.address = setting;
	data.expectedResponseCode = 0x89;
	prepareCommandReadInfo(data.requestData, data.id, data.command, setting);
	addOrUpdateFifo(data);
	return data.id;
}

void VEBus::SetSwitch(SwitchState state)
{
	Data data;
	data.responseExpected = false;
	//data.command //add stwichCommandNr
	prepareCommandSetSwitchState(data.requestData, state);
	addOrUpdateFifo(data);
}

const RAMVarInfo& VEBus::GetRamVarInfo(RamVariables variable)
{
	return _ramVarInfoList[variable];
}

const SettingInfo& VEBus::GetSettingInfo(Settings setting)
{
	return _settingInfoList[setting];
}

bool VEBus::NewMasterMultiLedAvailable()
{
	return _masterMultiLedNewData;
}

MasterMultiLed VEBus::GetMasterMultiLed()
{
	MasterMultiLed multiLed;
	xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
	_masterMultiLedNewData = false;
	multiLed = _masterMultiLed;
	xSemaphoreGive(_semaphoreStatus);

	return multiLed;
}

bool VEBus::NewMultiPlusStatusAvailable()
{
	return _multiPlusStatusNewData;
}

MultiPlusStatus VEBus::GetMultiPlusStatus()
{
	MultiPlusStatus multiStatus;
	xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
	_multiPlusStatusNewData = false;
	multiStatus = _multiPlusStatus;
	xSemaphoreGive(_semaphoreStatus);

	return multiStatus;
}

bool VEBus::NewDcInfoAvailable()
{
	return _dcInfo.newInfo;
}

DcInfo VEBus::GetDcInfo()
{
	DcInfo info;
	xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
	_dcInfo.newInfo = false;
	info = _dcInfo;
	xSemaphoreGive(_semaphoreStatus);
	return info;
}

AcInfo VEBus::GetAcInfo(uint8_t type)
{
	AcInfo info;
	xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
	info = _acInfo[PhaseToIdx(PhaseInfo(type))];
	_acInfo[PhaseToIdx(PhaseInfo(type))].newInfo = false;
	xSemaphoreGive(_semaphoreStatus);
	return info;
}

uint8_t VEBus::NewAcInfoAvailable()
{
	uint8_t phaseId = 0;
	for (auto& element : _acInfo) {
		if (element.newInfo == false) continue;
		phaseId = element.Phase;
		break;
	}

	return phaseId;
}

uint8_t VEBus::ReadSoftwareVersion()
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::SendSoftwareVersionPart0;
	data.address = 0;
	data.expectedResponseCode = 0x82;
	prepareCommandReadSoftwareVersion(data.requestData, data.id, data.command);
	addOrUpdateFifo(data);
	return data.id;
}

uint8_t VEBus::CommandReadDeviceState()
{
	Data data;
	if (!getNextFreeId_1(data.id)) return 0;
	data.responseExpected = true;
	data.command = WinmonCommand::GetSetDeviceState;
	data.address = 0;
	data.expectedResponseCode = 0x94;
	prepareCommandSetGetDeviceState(data.requestData, data.id, CommandDeviceState::Inquire);
	addOrUpdateFifo(data);
	return data.id;
}

void VEBus::addOrUpdateFifo(const Data &data, bool updateIfExist)
{
	xSemaphoreTake(_semaphoreDataFifo, portMAX_DELAY);
	if (updateIfExist)
	{
		for (auto& element : _dataFifo) {

			if (element.address == data.address && element.command == data.command)
			{
				element = data;
				element.responseData.clear();
				element.sentTimeMs = millis();
				xSemaphoreGive(_semaphoreDataFifo);
				return;
			}
		}
	}
	
	_dataFifo.push(data);
	xSemaphoreGive(_semaphoreDataFifo);
}

//possible ID_1 between 0x80 and 0xFF (0xE4-0xE7 used from Venus OS)
//* return false if no ID free
bool VEBus::getNextFreeId_1(uint8_t id)
{
	bool idUsed = false;
	for (uint8_t i = 0; i < 127; i++)
	{
		++_id;
		if (_id < 0x80) _id = 0x80;

		for (auto& element : _dataFifo) {

			if (element.id == _id)
			{
				idUsed = true;
				break;
			}
		}

		if (!idUsed)
		{
			id = _id;
			return true;
		}
	}

	return false;
}

void prepareCommand(VEBusBuffer &buffer, uint8_t frameNr)
{
	if (!buffer.resize(buffer.size() + 4)) {
		LogError("Failed to allocate enough data for command prefix");
		return;
	}
	for (int i = buffer.size() - 1; i >= 4; --i)
		buffer[i] = buffer[i - 4];
	buffer[0] = MK3_ID_0;
	buffer[1] = MK3_ID_1;
	buffer[2] = DATA_FRAME;
	buffer[3] = NEXT_FRAME_NR(frameNr);
}

void fill_command_buffer(VEBusBuffer &buffer, uint8_t id, uint8_t winmonCommand, StorageType storageType, uint8_t address, uint8_t lowByte, uint8_t highByte) {
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(WinmonCommand::WriteViaID);
	buffer.push(u8((winmonCommand == WinmonCommand::WriteRAMVar) ? VariableType::RamVar : VariableType::Setting) | u8(storageType)); // 0x02 -> no eeprom write
	buffer.push(address);
	buffer.push(lowByte);
	buffer.push(highByte);
}

// * address size up to 6
// Response: 0x85 / 0x90 < Lo(Value) > < Hi(Value)>  
// 0x85 = RamReadOK. 
// 0x90 = Variable not supported(in which case <Value> is not valid).
uint8_t prepareCommandReadMultiRAMVar(VEBusBuffer &buffer, uint8_t id, uint8_t* addresses, uint8_t addressSize)
{
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(WinmonCommand::ReadRAMVar);
	for (uint8_t i = 0; i < addressSize; i++)
	{
		buffer.push(addresses[i]);
	}

	return addressSize;
}

void prepareCommandReadSetting(VEBusBuffer &buffer, uint8_t id, uint16_t address)
{
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(WinmonCommand::ReadSetting);
	buffer.push(address & 0xFF);
	buffer.push(address >> 8);
}

void prepareCommandReadInfo(VEBusBuffer& buffer, uint8_t id, uint8_t winmonCommand, uint16_t setting)
{
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(winmonCommand);
	buffer.push(setting & 0xFF);
	buffer.push(setting >> 8);
}

//long Winmon frames
void prepareCommandReadSoftwareVersion(VEBusBuffer& buffer, uint8_t id, uint8_t winmonCommand)
{
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(winmonCommand);
}

void prepareCommandSetGetDeviceState(VEBusBuffer& buffer, uint8_t id, CommandDeviceState command, uint8_t state)
{
	buffer.clear();
	buffer.push(0x00);
	buffer.push(id);
	buffer.push(WinmonCommand::GetSetDeviceState);
	buffer.push(command);
	buffer.push(state);
}

//prepareCommand without ID
void prepareCommandSetSwitchState(VEBusBuffer& buffer, SwitchState switchState)
{
	buffer.clear();
	buffer.push(0x3F);
	buffer.push(switchState);
	buffer.push(0x00);
	buffer.push(0x00);
	buffer.push(0x00);
}

void stuffingFAtoFF(VEBusBuffer& buffer)
{
	uint32_t fas{};
	for (uint8_t v: buffer)
		if (v >= 0xFA)
			++fas;

	if (!buffer.resize(buffer.size() + fas)) {
		LogError("Failed to stuff FA to FF");
		return;
	}
		
	for (uint8_t *dst = buffer.back(), *src = dst - fas; src && src >= buffer.begin(); --src)
	{
		if (*src >= 0xFA)
		{
			*dst-- = 0x70 | (*src & 0x0F);
			*dst-- = 0xFA;
		} else
			*dst-- = *src;
	}
}

void DestuffingFAtoFF(VEBusBuffer& buffer)
{
	if (buffer.empty())
		return;

	uint32_t fas{};
	for (uint8_t v: buffer)
		if (v == 0xFA)
			++fas;

	if (*buffer.back() == 0xFA)
		--fas;

	for (uint8_t *src = buffer.back(), *dst = dst - fas; src && src >= buffer.begin(); --dst)
	{
		if (src != buffer.begin() && *(src - 1) == 0xFA)
		{
			*dst = 0x80 + *src;
			src -= 2;
		} else
			*dst = *src--;
	}
	std::ignore = buffer.resize(buffer.size() - fas);
}

void appendChecksum(VEBusBuffer& buffer)
{
	//calculate checksum without MK3_ID
	uint8_t cs = 1;
	if (buffer.size() < 2) return;

	for (int i = 2; i < buffer.size(); i++) cs -= buffer[i];

	if (cs >= 0xFB)
	{
		buffer.push(0xFA);
		buffer.push(cs - 0xFA);
	}
	else
	{
		buffer.push(cs);
	}

	buffer.push(END_OF_FRAME);  //End Of Frame
}

uint16_t convertRamVarToRawValue(RamVariables variable, float value, const RAMVarInfos &ramVarInfoList)
{
	uint16_t rawValue;
	int16_t scale = abs(ramVarInfoList[variable].Scale);
	if (scale >= 0x4000) scale = (0x8000 - scale);

	rawValue = value * (scale + 0.0f);
	rawValue -= ramVarInfoList[variable].Offset;
	return rawValue;
}

float convertRamVarToValue(RamVariables variable, uint16_t rawValue, const RAMVarInfos &ramVarInfoList)
{
	float value;
	int16_t scale = abs(ramVarInfoList[variable].Scale);
	if (scale >= 0x4000) scale = (0x8000 - scale);

	value = rawValue / (scale + 0.0f);
	value += ramVarInfoList[variable].Offset;
	return value;
}

int16_t convertRamVarToRawValueSigned(RamVariables variable, float value, const RAMVarInfos &ramVarInfoList)
{
	int16_t rawValue;
	int16_t scale = abs(ramVarInfoList[variable].Scale);
	if (scale >= 0x4000) scale = (0x8000 - scale);

	rawValue = value * (scale + 0.0f);
	rawValue -= ramVarInfoList[variable].Offset;
	return rawValue;
}

float convertRamVarToValueSigned(RamVariables variable, int16_t rawValue, const RAMVarInfos &ramVarInfoList)
{
	float value;
	int16_t scale = abs(ramVarInfoList[variable].Scale);
	if (scale >= 0x4000) scale = (0x8000 - scale);

	value = rawValue / (scale + 0.0f);
	value += ramVarInfoList[variable].Offset;
	return value;
}

uint16_t convertSettingToRawValue(Settings setting, float value, const SettingInfos &settingInfoList)
{
	uint16_t rawValue;
	if (settingInfoList[setting].Scale > 0) rawValue =  (uint16_t)(value / settingInfoList[setting].Scale);
	else rawValue = (uint16_t)(value / (1.0f / -(settingInfoList[setting].Scale)));

	rawValue -= settingInfoList[setting].Offset;
	return rawValue;
}

float convertSettingToValue(Settings setting, uint16_t rawValue, const SettingInfos &settingInfoList)
{
	float value;
	if (settingInfoList[setting].Scale > 0) value = rawValue * settingInfoList[setting].Scale;
	else value = rawValue * (1.0f / -(settingInfoList[setting].Scale));

	value += settingInfoList[setting].Offset;
	return value;
}










//Runs on core 0
ReceivedMessageType VEBus::decodeVEbusFrame(VEBusBuffer& buffer)
{
	ReceivedMessageType result = ReceivedMessageType::Unknown;
	if ((buffer[0] != MP_ID_0) || (buffer[1] != MP_ID_1)) return ReceivedMessageType::Unknown;
	if ((buffer[2] == SYNC_FRAME) && (buffer.size() == 10) && (buffer[4] == SYNC_BYTE)) return ReceivedMessageType::sync;
	if (buffer[2] != DATA_FRAME) return ReceivedMessageType::Unknown;

	switch (buffer[4]) {
	case 0x00:
	{
		if (buffer.size() < 6) return ReceivedMessageType::Unknown;
		xSemaphoreTake(_semaphoreDataFifo, portMAX_DELAY);
		for (uint8_t i = 0; i < _dataFifo.size(); i++)
		{
			if (_dataFifo[i].id != buffer[5]) continue;
			_dataFifo[i].responseData = buffer;
			break;
		}
		xSemaphoreGive(_semaphoreDataFifo);
		break;
	}
	case 0x20: //Info Frame
	{
		if (buffer.size() < 20) return ReceivedMessageType::Unknown;
		decodeInfoFrame(buffer);
		break;
	}
	case 0x41:
	{
		if ((buffer.size() == 19) && (buffer[5] == 0x10))
		{
			decodeMasterMultiLed(buffer);
			result = ReceivedMessageType::Known;
		}
		break;
	}
	case 0x70:
	{
		if ((buffer.size() == 15) && (buffer[5] == 0x81) && (buffer[6] == 0x64) && (buffer[7] == 0x14) && (buffer[8] == 0xBC) && (buffer[9] == 0x02) && (buffer[12] == 0x00))
		{
			decodeBatteryCondition(buffer);
			result = ReceivedMessageType::Known;
		}
		break;
	}
	case 0x80:
	{
		decodeChargerInverterCondition(buffer);
		result = ReceivedMessageType::Known;
		break;
	}
	case 0xE4:
	{
		if (buffer.size() == 21) result = ReceivedMessageType::AcPhaseInformation;
		break;
	}
	}

	return result;
}

void VEBus::decodeChargerInverterCondition(VEBusBuffer& buffer)
{
	if ((buffer.size() == 19) && (buffer[5] == 0x80) && ((buffer[6] & 0xFE) == 0x12) && (buffer[8] == 0x80) && ((buffer[11] & 0x10) == 0x10) && (buffer[12] == 0x00))
	{
		if (_masterMultiLed.LowBattery != (buffer[7] == LOW_BATTERY))
		{
			xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
			_masterMultiLed.LowBattery = (buffer[7] == LOW_BATTERY);
			_masterMultiLedNewData = true;
			_masterMultiLedLogged = false;
			xSemaphoreGive(_semaphoreStatus);

		}

		bool dcLevelAllowsInverting = (buffer[6] & 0x01);
		float dcCurrentA = (((uint16_t)buffer[10] << 8) | buffer[9]) / 10.0f;
		float temp = 0;
		if ((buffer[11] & 0xF0) == 0x30) temp = buffer[15] / 10.0f;

		bool newValue = false;
		newValue |= _multiPlusStatus.DcLevelAllowsInverting != dcLevelAllowsInverting;
		newValue |= _multiPlusStatus.DcCurrentA != dcCurrentA;
		if ((buffer[11] & 0xF0) == 0x30) newValue |= _multiPlusStatus.Temp != temp;

		if (newValue)
		{
			xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
			_multiPlusStatus.DcLevelAllowsInverting = dcLevelAllowsInverting;
			_multiPlusStatus.DcCurrentA = dcCurrentA;
			_multiPlusStatusNewData = true;
			_multiPlusStatusLogged = false;
			if ((buffer[11] & 0xF0) == 0x30) _multiPlusStatus.Temp = temp;
			xSemaphoreGive(_semaphoreStatus);
		}
	}
}

void VEBus::decodeBatteryCondition(VEBusBuffer& buffer)
{
	if ((buffer.size() == 15) && (buffer[5] == 0x81) && (buffer[6] == 0x64) && (buffer[7] == 0x14) && (buffer[8] == 0xBC) && (buffer[9] == 0x02) && (buffer[12] == 0x00))
	{
		float multiplusAh = (((uint16_t)buffer[11] << 8) | buffer[10]);
		if (multiplusAh != _multiPlusStatus.BatterieAh)
		{
			xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
			_multiPlusStatus.BatterieAh = multiplusAh;
			_multiPlusStatusNewData = true;
			_multiPlusStatusLogged = false;
			xSemaphoreGive(_semaphoreStatus);
		}
	}
}

void VEBus::decodeMasterMultiLed(VEBusBuffer& buffer)
{
	LEDData lEDon{};
	LEDData lEDblink{};
	lEDon.value = buffer[6];
	lEDblink.value = buffer[7];
	bool lowBattery = (buffer[8] == LOW_BATTERY);
	uint8_t lED_AcInputConfiguration = buffer[9];
	float minimumInputCurrentLimit = (((uint16_t)buffer[11] << 8) | buffer[10]) / 10.0f;
	float maximumInputCurrentLimit = (((uint16_t)buffer[13] << 8) | buffer[12]) / 10.0f;
	float actualInputCurrentLimit = (((uint16_t)buffer[15] << 8) | buffer[14]) / 10.0f;
	uint8_t switchRegister = buffer[16];

	bool newValue = false;

	newValue |= _masterMultiLed.LEDon.value != lEDon.value;
	newValue |= _masterMultiLed.LEDblink.value != lEDblink.value;
	newValue |= _masterMultiLed.LowBattery != lowBattery;
	newValue |= _masterMultiLed.AcInputConfiguration != lED_AcInputConfiguration;
	newValue |= _masterMultiLed.MinimumInputCurrentLimitA != minimumInputCurrentLimit;
	newValue |= _masterMultiLed.MaximumInputCurrentLimitA != maximumInputCurrentLimit;
	newValue |= _masterMultiLed.ActualInputCurrentLimitA != actualInputCurrentLimit;
	newValue |= _masterMultiLed.SwitchRegister != switchRegister;

	if (newValue)
	{
		xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
		_masterMultiLed.LEDon.value = lEDon.value;
		_masterMultiLed.LEDblink.value = lEDblink.value;
		_masterMultiLed.LowBattery = lowBattery;
		_masterMultiLed.AcInputConfiguration = lED_AcInputConfiguration;
		_masterMultiLed.MinimumInputCurrentLimitA = minimumInputCurrentLimit;
		_masterMultiLed.MaximumInputCurrentLimitA = maximumInputCurrentLimit;
		_masterMultiLed.ActualInputCurrentLimitA = actualInputCurrentLimit;
		_masterMultiLed.SwitchRegister = switchRegister;
		_masterMultiLedNewData = true;
		_masterMultiLedLogged = false;
		xSemaphoreGive(_semaphoreStatus);
	}
}

void VEBus::decodeInfoFrame(VEBusBuffer &buffer)
{
	if (buffer.size() < 18) {
		LogError("decodeInfoFrame too small buffer");
		return;
	}
	switch (buffer[9])
	{
	case VEBusDefinition::L4:
	case VEBusDefinition::L3:
	case VEBusDefinition::L2:
	case VEBusDefinition::S_L1: // 83 83 FE 1B 20 01 01 00 04 08 00 00 00 00 C6 59 1E 00 00 7D FF 
	case VEBusDefinition::S_L2:
	case VEBusDefinition::S_L3:
	case VEBusDefinition::S_L4:
	{
		AcInfo info{};
		info.Phase = (PhaseInfo)buffer[9];
		info.State = (PhaseState)buffer[8];
		info.MainVoltage = convertRamVarToValueSigned(RamVariables::UBat, (buffer[11] << 8 | buffer[10]), _ramVarInfoList);
		info.MainCurrent = convertRamVarToValueSigned(RamVariables::IInverterRMS, (buffer[13] << 8 | buffer[12]), _ramVarInfoList) * buffer[5]; // buffer[5] -> BF factor
		info.InverterVoltage = convertRamVarToValueSigned(RamVariables::UBat, (buffer[15] << 8 | buffer[14]), _ramVarInfoList);
		info.InverterCurrent = convertRamVarToValueSigned(RamVariables::IInverterRMS, (buffer[17] << 8 | buffer[16]), _ramVarInfoList) * buffer[6]; // buffer[6] -> Inverter factor
		//info.MainFrequency = convertSettingToValue(Settings::RepeatedAbsorptionTime,buffer[18]);

		AcInfo &ac_entry = _acInfo[PhaseToIdx(info.Phase)];
		if (info == ac_entry)
			return;
		ac_entry.newInfo = true;
		xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
		ac_entry = info;
		xSemaphoreGive(_semaphoreStatus);
		break;
	}
	case VEBusDefinition::DC: // 83 83 FE 72 20 40 A5 C4 01 0C 33 05 12 00 00 00 00 00 86 EB FF
	{
		DcInfo info{};
		info.Voltage = convertRamVarToValueSigned(RamVariables::UBat, (buffer[11] << 8 | buffer[10]), _ramVarInfoList);
		info.CurrentInverting = convertRamVarToValueSigned(RamVariables::IBat, (buffer[12] | (buffer[13] << 8) | (buffer[14] << 16)), _ramVarInfoList);
		info.CurrentCharging = convertRamVarToValueSigned(RamVariables::IBat, (buffer[15] | (buffer[16] << 8) | (buffer[17] << 16)), _ramVarInfoList);
		//info.InverterFrequency = 1 / convertSettingToValue(Settings::RepeatedAbsorptionTime, buffer[18]) * 10;

		if (info == _dcInfo) break;
		info.newInfo = true;
		xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);
		_dcInfo = info;
		xSemaphoreGive(_semaphoreStatus);
		break;
	}
	default:
		break;
	}
	xSemaphoreTake(_semaphoreStatus, portMAX_DELAY);

	xSemaphoreGive(_semaphoreStatus);
}

//Runs on core 0
void VEBus::commandHandling()
{
	if (!_communitationIsRunning) 
		return;

	if (_communitationIsResumed)
	{
		_communitationIsResumed = false;
		_serial.tx_flush();
	}

	if (!_serial.rx_available()) 
		return;

	while(_serial.rx_available()) {
		char c = _serial.getc();
		_receiveBuffer.push(c);
		if (c == END_OF_FRAME)
			break;
	}
	if (*_receiveBuffer.back() != END_OF_FRAME) 
		return;

	xSemaphoreTake(_semaphoreReceiveData, portMAX_DELAY);
	_receiveBufferList.push(_receiveBuffer);
	xSemaphoreGive(_semaphoreReceiveData);

	DestuffingFAtoFF(_receiveBuffer);
	auto messageType = decodeVEbusFrame(_receiveBuffer);
	uint8_t frameNr = _receiveBuffer[3];
	_receiveBuffer.clear();

	// check for sync frame and frames have to be sent
	if (messageType != ReceivedMessageType::sync || _dataFifo.empty())
		return;

	// we can now transmit a request that was not yet sent
	xSemaphoreTake(_semaphoreDataFifo, portMAX_DELAY);
	Data* data{};
	for (Data &d: _dataFifo) {
		if (!d.IsSent) {
			data = &d;
			break;
		}
	}
	if (data)
		sendData(*data, frameNr);

	if (data && !data->responseExpected)
		*data = *_dataFifo.pop();

	xSemaphoreGive(_semaphoreDataFifo);
}

void VEBus::sendData(VEBus::Data& data, uint8_t frameNr)
{
	VEBusBuffer sendData = data.requestData;
	prepareCommand(sendData, frameNr);
	stuffingFAtoFF(sendData);
	appendChecksum(sendData);

	_serial.enable_send();
	_serial.write(sendData.begin(), sendData.size());
	_serial.tx_flush();
	_serial.enable_receive();

	data.sentTimeMs = millis();
	data.IsSent = true;
	data.IsLogged = false;
}

void VEBus::checkResponseMessage()
{
	Data *data{};
	xSemaphoreTake(_semaphoreDataFifo, portMAX_DELAY);
	for (int i = _dataFifo.back_idx(); i >= 0; --i)
	{
		if (_dataFifo[i].responseData.size() == 0)
			continue;

		if (_dataFifo[i].responseData[6] == _dataFifo[i].expectedResponseCode)
		{
			std::swap(_dataFifo[i], *_dataFifo.back()); // swap to ensure sent data is removed
			data = _dataFifo.pop(); // erase last element and get pointer
			break;
		}

		if (_dataFifo[i].resendCount >= VEBUS_MAX_RESEND) {
			_dataFifo[i] = *_dataFifo.pop();
			break;
		}
		else {
			_dataFifo[i].resendCount++;
			_dataFifo[i].IsSent = false;
			_dataFifo[i].sentTimeMs = millis();
			break;
		}
	}
	xSemaphoreGive(_semaphoreDataFifo);

	if (data) 
		saveResponseData(*data);
}

void VEBus::saveResponseData(const Data &data)
{
	bool callResponseCb = false;
	ResponseData responseData;
	responseData.id = data.id;
	responseData.command = data.command;
	responseData.address = data.address;

	switch (data.command)
	{
	case VEBusDefinition::SendSoftwareVersionPart0:
	{
		if (data.responseData.size() != 19) {
			LogWarning("SendSoftwareVersionPart0 wrong size {}", data.responseData.size());
			break;
		}
		callResponseCb = true;
		responseData.value = u32((data.responseData[7]) | (data.responseData[8] << 8) | (data.responseData[9] << 16) | (data.responseData[10] << 24));
		//[11] [12] [13] [14] [15] [16] still unknown
		// 08   1D 	 00   00   39   10
		break;
	}
	case VEBusDefinition::SendSoftwareVersionPart1:
		break;
	case VEBusDefinition::GetSetDeviceState:
		if (data.responseData.size() != 11) {
			LogWarning("GetSetDeviceState wrong size {}", data.responseData.size());
			break;
		}
		callResponseCb = true;
		if (data.responseData[7] == 9) responseData.value = u32(data.responseData[7] + data.responseData[8]);
		else responseData.value = u32(data.responseData[7]);
		break;
	case VEBusDefinition::ReadRAMVar:
	{
		if (data.responseData.size() != 11) {
			LogWarning("ReadRAMVar wrong size {}", data.responseData.size());
			break;
		}
		callResponseCb = true;
		uint16_t UnsignedRawValue = (((uint16_t)data.responseData[8] << 8) | data.responseData[7]);
		int16_t signedRawValueint = ((int16_t)data.responseData[8] << 8) | data.responseData[7];
		if (!_ramVarInfoList[data.address].available) break;
		switch (_ramVarInfoList[data.address].dataType)
		{
		case VEBusDefinition::none:
			break;
		case VEBusDefinition::floatingPoint:
			if (_ramVarInfoList[data.address].Scale < 0) responseData.value = float(convertRamVarToValueSigned((RamVariables)data.address, signedRawValueint, _ramVarInfoList));
			else responseData.value = f32(convertRamVarToValue((RamVariables)data.address, UnsignedRawValue, _ramVarInfoList));

			std::get<f32>(responseData.value) += _ramVarInfoList[data.address].Offset;
			break;
		case VEBusDefinition::unsignedInteger:
			responseData.value = u32(UnsignedRawValue);
			break;
		case VEBusDefinition::signedInteger:
			responseData.value = i32(signedRawValueint);
			break;
		default:
			break;
		}
		
		break;
	}
	case VEBusDefinition::ReadSetting:
	{
		if (data.responseData.size() != 11) {
			LogWarning("ReadSetting wrong size {}", data.responseData.size());
			break;
		}
		callResponseCb = true;
		uint16_t rawValue;
		rawValue = ((uint16_t)data.responseData[8] << 8) | data.responseData[7];
		if (!_settingInfoList[data.address].available) break;
		switch (_settingInfoList[data.address].dataType)
		{
		case VEBusDefinition::none:
			break;
		case VEBusDefinition::floatingPoint:
			responseData.value = f32(convertSettingToValue((Settings)data.address, rawValue, _settingInfoList));
			break;
		case VEBusDefinition::unsignedInteger:
			responseData.value = i32(rawValue);
			break;
		default:
			break;
		}
		
		break;
	}
	case VEBusDefinition::WriteRAMVar:
		break;
	case VEBusDefinition::WriteSetting:
		break;
	case VEBusDefinition::WriteData:
		break;
	case VEBusDefinition::GetSettingInfo:
		if (data.responseData.size() != 20) {
			LogWarning("GetSettingInfo wrong size {}", data.responseData.size());
			break;
		}
		saveSettingInfoData(data);
		break;
	case VEBusDefinition::GetRAMVarInfo:
		if (data.responseData.size() != 13) {
			LogWarning("GetRAMVarInfo wrong size {}", data.responseData.size());
			break;
		}
		saveRamVarInfoData(data);
		break;
	case VEBusDefinition::WriteViaID:
		break;
	case VEBusDefinition::ReadSnapShot:
		break;
	default:
		break;
	}

	if (callResponseCb && response_cb)
		response_cb(responseData);

	LogInfo("Res: {}", data.responseData);
}

void VEBus::saveSettingInfoData(const Data& data)
{
	SettingInfo settingInfo;
	settingInfo.Scale = ((int16_t)data.responseData[8] << 8) | data.responseData[7];
	settingInfo.Offset = ((int16_t)data.responseData[10] << 8) | data.responseData[9];
	settingInfo.Default = ((uint16_t)data.responseData[12] << 8) | data.responseData[11];
	settingInfo.Minimum = ((uint16_t)data.responseData[14] << 8) | data.responseData[13];
	settingInfo.Maximum = ((uint16_t)data.responseData[16] << 8) | data.responseData[15];
	settingInfo.AccessLevel = data.responseData[17];
	_settingInfoList[data.address] = settingInfo;

	LogInfo("SettingInfo {}, sc: {} offset: {}, default: {}, min: {}, max: {}, access: {}", data.address, settingInfo.Scale, settingInfo.Offset, settingInfo.Default, settingInfo.Minimum, settingInfo.Maximum, settingInfo.AccessLevel);
}

void VEBus::saveRamVarInfoData(const Data& data)
{
	RAMVarInfo ramVarInfo;
	ramVarInfo.Scale = ((int16_t)data.responseData[8] << 8) | data.responseData[7];
	ramVarInfo.Offset = ((int16_t)data.responseData[10] << 8) | data.responseData[9];
	_ramVarInfoList[data.address] = ramVarInfo;

	LogInfo("RamVarInfo {}, sc: {}, offset: {}", data.address, ramVarInfo.Scale, ramVarInfo.Offset);
}

void VEBus::checkResponseTimeout()
{

	xSemaphoreTake(_semaphoreDataFifo, portMAX_DELAY);
	if (_dataFifo.empty()) {
		xSemaphoreGive(_semaphoreDataFifo);
		return;
	}

	for (int i = _dataFifo.back_idx(); i >= 0; --i)
	{
		Data &d = _dataFifo[i];
		if (millis() - d.sentTimeMs < VEBUS_RESPONSE_TIMEOUT)
			continue;
		LogWarning("Timeout id: {} command {} resend count: {}", d.id, d.command, d.resendCount);
		if (d.resendCount >= VEBUS_MAX_RESEND) {
			std::swap(d, *_dataFifo.pop());
			LogWarning("The message is deleted.");
		}
		else {
			d.resendCount++;
			d.IsSent = false;
			d.sentTimeMs = millis();
		}
	}
	xSemaphoreGive(_semaphoreDataFifo);
}

