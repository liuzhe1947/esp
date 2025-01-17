
#include "Config.h"
#include "Debug.h"
#include "template.h"
#include <EEPROM.h>
#include <FS.h>
#include <Ticker.h>
#include <pb_encode.h>
#include <pb_decode.h>
#include "config.pb.h"

Module *module;
char UID[16];
char tmpData[512] = {0};
uint8_t GPIO_PIN[20];
uint32_t perSecond;
Ticker *tickerPerSecond;
ConfigMessage config;

uint16_t Config::nowCrc;

const uint16_t crcTalbe[] = {
	0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
	0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};

/**
 * 计算Crc16
 */
uint16_t Config::crc16(uint8_t *ptr, uint16_t len)
{
	uint16_t crc = 0xffff;
	for (uint16_t i = 0; i < len; i++)
	{
		const uint8_t ch = *ptr++;
		crc = crcTalbe[(ch ^ crc) & 15] ^ (crc >> 4);
		crc = crcTalbe[((ch >> 4) ^ crc) & 15] ^ (crc >> 4);
	}
	return crc;
}

bool Config::resetConfig()
{
	Debug.AddLog(LOG_LEVEL_INFO, PSTR("resetConfig . . . OK"));
	memset(&config, 0, sizeof(ConfigMessage));

#ifdef WIFI_SSID
	strcpy(config.wifi_ssid, WIFI_SSID);
#endif
#ifdef WIFI_PASS
	strcpy(config.wifi_pass, WIFI_PASS);

#endif
#ifdef MQTT_SERVER
	strcpy(config.mqtt_server, MQTT_SERVER);
#endif
#ifdef MQTT_PORT
	config.mqtt_port = MQTT_PORT;
#endif
#ifdef MQTT_USER
	strcpy(config.mqtt_user, MQTT_USER);
#endif
#ifdef MQTT_PASS
	strcpy(config.mqtt_pass, MQTT_PASS);
#endif
	config.mqtt_discovery = false;
	strcpy(config.mqtt_discovery_prefix, "homeassistant");

#ifdef MQTT_FULLTOPIC
	strcpy(config.mqtt_topic, MQTT_FULLTOPIC);
#endif
#ifdef OTA_URL
	strcpy(config.ota_url, OTA_URL);
#endif
	config.http_port = 80;

#ifdef USE_RELAY
	config.module_type = SupportedModules::CH3;

	config.relay_led_light = 50;
	config.relay_led_time = 3;
#elif defined USE_COVER
	config.module_type = SupportedModules::HUEX_COVER;

	config.cover_position = 127;
	config.cover_direction = 127;
	config.cover_hand_pull = 127;
	config.cover_weak_switch = 127;
	config.cover_power_switch = 127;

#elif defined USE_ZINGUO
	config.module_type = SupportedModules::ZINGUO;

	config.zinguo_dual_motor = true;
	config.zinguo_dual_warm = true;
	config.zinguo_delay_blow = 30;
	config.zinguo_linkage = 1;
	config.zinguo_max_temp = 40;
	config.zinguo_close_warm = 30;
	config.zinguo_close_ventilation = 30;
	config.zinguo_beep = true;
#else
#error "not support module"
#endif

	config.debug_type = 1;
	return true;
}

bool Config::readConfig(bool isErrorReset)
{
	uint16 len;
	bool status = false;
	uint16 cfg = (EEPROM.read(0) << 8 | EEPROM.read(1));
	if (cfg == CFG_HOLDER)
	{
		len = (EEPROM.read(2) << 8 | EEPROM.read(3));
		nowCrc = (EEPROM.read(4) << 8 | EEPROM.read(5));

		if (len > ConfigMessage_size)
		{
			len = ConfigMessage_size;
		}

		uint16_t crc = 0xffff;
		uint8_t buffer[ConfigMessage_size];
		for (uint16_t i = 0; i < len; ++i)
		{
			buffer[i] = EEPROM.read(i + 6);

			crc = crcTalbe[(buffer[i] ^ crc) & 15] ^ (crc >> 4);
			crc = crcTalbe[((buffer[i] >> 4) ^ crc) & 15] ^ (crc >> 4);
		}
		if (crc == nowCrc)
		{
			memset(&config, 0, sizeof(ConfigMessage));
			pb_istream_t stream = pb_istream_from_buffer(buffer, len);
			status = pb_decode(&stream, ConfigMessage_fields, &config);
			if (config.http_port == 0)
			{
				config.http_port = 80;
			}
		}
	}

	if (!status)
	{
		config.debug_type = 1;
		Debug.AddLog(LOG_LEVEL_INFO, PSTR("readConfig . . . Error"));
		if (isErrorReset)
		{
			resetConfig();
			return true;
		}
		memset(&config, 0, sizeof(ConfigMessage));
		return false;
	}

	Debug.AddLog(LOG_LEVEL_INFO, PSTR("readConfig . . . OK Len: %d"), len);
	return true;
}

bool Config::saveConfig()
{
	uint8_t buffer[ConfigMessage_size];
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
	bool status = pb_encode(&stream, ConfigMessage_fields, &config);
	size_t len = stream.bytes_written;
	if (!status)
	{
		Debug.AddLog(LOG_LEVEL_INFO, PSTR("saveConfig . . . Error"));
		return false;
	}

	nowCrc = crc16(buffer, len);

	EEPROM.write(0, CFG_HOLDER >> 8);
	EEPROM.write(1, CFG_HOLDER);

	EEPROM.write(2, len >> 8);
	EEPROM.write(3, len);

	EEPROM.write(4, nowCrc >> 8);
	EEPROM.write(5, nowCrc);

	for (uint16_t i = 0; i < len; i++)
	{
		EEPROM.write(i + 6, buffer[i]);
	}
	EEPROM.commit();

	Debug.AddLog(LOG_LEVEL_INFO, PSTR("saveConfig . . . OK Len: %d"), len);
	return true;
}

void Config::perSecondDo()
{
	if (perSecond % 60 != 0)
	{
		return;
	}
	uint8_t buffer[ConfigMessage_size];
	pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
	bool status = pb_encode(&stream, ConfigMessage_fields, &config);
	size_t len = stream.bytes_written;
	if (!status)
	{
		Debug.AddLog(LOG_LEVEL_INFO, PSTR("Check Config CRC . . . Error"));
	}
	else
	{
		uint16_t crc = crc16(buffer, len);
		if (crc != nowCrc)
		{
			Debug.AddLog(LOG_LEVEL_INFO, PSTR("Check Config CRC . . . Different"));
			saveConfig();
		}
		else
		{
			//Debug.AddLog(LOG_LEVEL_INFO, PSTR("Check Config CRC . . . OK"));
		}
	}
}