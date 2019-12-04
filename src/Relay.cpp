#ifdef USE_RELAY

#include "Debug.h"
#include "Relay.h"
#include "RelayButton.h"
#include "RadioReceive.h"
#include "Config.h"
#include "Mqtt.h"
#include "Ticker.h"
#include "Http.h"
#include "Ntp.h"

String Relay::moduleName()
{
	return "sonoff";
}

void Relay::ledTickerHandle()
{
	for (uint8_t ch = 0; ch < Relay::channels; ch++)
	{
		if (!lastState[ch] && GPIO_PIN[GPIO_LED1 + ch] != 99)
		{
			analogWrite(GPIO_PIN[GPIO_LED1 + ch], ledLevel);
		}
	}
	if (ledUp)
	{
		ledLevel++;
		if (ledLevel >= ledLight)
		{
			ledUp = false;
		}
	}
	else
	{
		ledLevel--;
		if (ledLevel <= 50)
		{
			ledUp = true;
		}
	}
}

void Relay::ledPWM(uint8_t ch, bool isOn)
{
	if (isOn)
	{
		analogWrite(GPIO_PIN[GPIO_LED1 + ch], 0);
		if (ledTicker->active())
		{
			for (uint8_t ch2 = 0; ch2 < Relay::channels; ch2++)
			{
				if (!lastState[ch2])
				{
					return;
				}
			}
			ledTicker->detach();
			Debug.AddLog(LOG_LEVEL_INFO, PSTR("ledTicker detach"));
		}
	}
	else
	{
		if (!ledTicker->active())
		{
			ledTicker->attach_ms(config.relay_led_time, std::bind(&Relay::ledTickerHandle, this));
			Debug.AddLog(LOG_LEVEL_INFO, PSTR("ledTicker active"));
		}
	}
}

void Relay::led(uint8_t ch, bool isOn)
{
	if (config.relay_led_type == 0 || GPIO_PIN[GPIO_LED1 + ch] == 99)
	{
		return;
	}

	if (config.relay_led_type == 1)
	{
		//digitalWrite(GPIO_PIN[GPIO_LED1 + ch], isOn ? LOW : HIGH);
		analogWrite(GPIO_PIN[GPIO_LED1 + ch], isOn ? 0 : ledLight);
	}
	else if (config.relay_led_type == 2)
	{
		ledPWM(ch, isOn);
	}
}

boolean Relay::checkCanLed(boolean re)
{
	boolean result;
	if (config.relay_led_start != config.relay_led_end && Ntp::rtcTime.valid)
	{
		uint16_t nowTime = Ntp::rtcTime.hour * 100 + Ntp::rtcTime.minute;
		if (config.relay_led_start > config.relay_led_end) // 开始时间大于结束时间 跨日
		{
			result = (nowTime >= config.relay_led_start || nowTime < config.relay_led_end);
		}
		else
		{
			result = (nowTime >= config.relay_led_start && nowTime < config.relay_led_end);
		}
	}
	else
	{
		result = true; // 没有正确时间为一直亮
	}
	if (result != Relay::canLed || re)
	{
		if ((!result || config.relay_led_type != 2) && ledTicker && ledTicker->active())
		{
			ledTicker->detach();
			Debug.AddLog(LOG_LEVEL_INFO, PSTR("ledTicker detach2"));
		}
		Relay::canLed = result;
		Debug.AddLog(LOG_LEVEL_INFO, result ? PSTR("led can light") : PSTR("led can not light"));
		for (uint8_t ch = 0; ch < Relay::channels; ch++)
		{
			if (GPIO_PIN[GPIO_LED1 + ch] != 99)
			{
				result &&config.relay_led_type != 0 ? led(ch, lastState[ch]) : analogWrite(GPIO_PIN[GPIO_LED1 + ch], 0);
			}
		}
	}

	return result;
}

void Relay::switchRelay(uint8_t ch, bool isOn, bool isSave)
{
	if (ch > Relay::channels)
	{
		Debug.AddLog(LOG_LEVEL_INFO, PSTR("invalid channel: %d"), ch);
		return;
	}
	Debug.AddLog(LOG_LEVEL_INFO, PSTR("Relay %d . . . %s"), ch + 1, isOn ? "ON" : "OFF");

	lastState[ch] = isOn;
	digitalWrite(GPIO_PIN[GPIO_REL1 + ch], isOn ? HIGH : LOW);

	mqtt->publish(Relay::channels == 1 ? powerTopic : (powerTopic + (ch + 1)), isOn ? "ON" : "OFF", config.mqtt_retain);

	if (isSave && config.relay_power_on_state > 0)
	{
		config.relay_last_state[ch] = isOn;
	}
	if (Relay::canLed)
	{
		led(ch, isOn);
	}
}

void Relay::mqttCallback(String topicStr, String str)
{
	if (Relay::channels >= 1 && topicStr.endsWith("/POWER") || topicStr.endsWith("/POWER1"))
	{
		switchRelay(0, (str == "ON" ? true : (str == "OFF" ? false : !Relay::lastState[0])));
	}
	else if (Relay::channels >= 2 && topicStr.endsWith("/POWER2"))
	{
		switchRelay(1, (str == "ON" ? true : (str == "OFF" ? false : !Relay::lastState[1])));
	}
	else if (Relay::channels >= 3 && topicStr.endsWith("/POWER3"))
	{
		switchRelay(2, (str == "ON" ? true : (str == "OFF" ? false : !Relay::lastState[2])));
	}
	else if (Relay::channels >= 4 && topicStr.endsWith("/POWER4"))
	{
		switchRelay(3, (str == "ON" ? true : (str == "OFF" ? false : !Relay::lastState[3])));
	}
}

void Relay::handleRelaySet(ESP8266WebServer *server)
{
	String c = server->arg(F("c"));
	if (c != F("1") && c != F("2") && c != F("3") && c != F("4"))
	{
		server->send(200, F("text/html"), F("{\"code\":0,\"msg\":\"参数错误。\"}"));
		return;
	}
	uint8_t ch = c.toInt() - 1;
	if (ch > Relay::channels)
	{
		server->send(200, F("text/html"), F("{\"code\":0,\"msg\":\"继电器数量错误。\"}"));
		return;
	}
	String str = server->arg(F("do"));
	switchRelay(ch, (str == "ON" ? true : (str == "OFF" ? false : !Relay::lastState[ch])));

	server->send(200, F("text/html"), "{\"code\":1,\"msg\":\"操作成功\",\"data\":{" + httpGetStatus(server) + "}}");
}

void Relay::handleRadioReceive(ESP8266WebServer *server)
{
	String c = server->arg(F("c"));
	if (c == F("0"))
	{
		config.relay_study_index[0] = 0;
		config.relay_study_index[1] = 0;
		config.relay_study_index[2] = 0;
		config.relay_study_index[3] = 0;
		Config::saveConfig();
	}
	else if (c == F("1") || c == F("2") || c == F("3") || c == F("4"))
	{
		config.relay_study_index[c.toInt() - 1] = 0;
		Config::saveConfig();
	}
	else
	{
		server->send(200, F("text/html"), F("{\"code\":0,\"msg\":\"参数错误。\"}"));
		return;
	}
	server->send(200, F("text/html"), F("{\"code\":1,\"msg\":\"操作成功\"}"));
}

void Relay::handleRelaySetting(ESP8266WebServer *server)
{
	if (server->hasArg(F("power_on_state")))
	{
		String powerOnState = server->arg(F("power_on_state"));
		if (powerOnState != F("0") || powerOnState != F("1") || powerOnState != F("2") || powerOnState != F("3"))
		{
			config.relay_power_on_state = powerOnState.toInt();
		}
	}

	if (server->hasArg(F("led_type")))
	{
		String ledType = server->arg(F("led_type"));
		config.relay_led_type = ledType.toInt();

		if (config.relay_led_type == 2 && !ledTicker)
		{
			ledTicker = new Ticker();
		}
	}
	if (server->hasArg(F("led_start")) && server->hasArg(F("led_end")))
	{
		String ledStart = server->arg(F("led_start"));
		String ledEnd = server->arg(F("led_end"));
		config.relay_led_start = ledStart.toInt();
		config.relay_led_end = ledEnd.toInt();
	}
	if (server->hasArg(F("led_light")))
	{
		config.relay_led_light = server->arg(F("led_light")).toInt();
		ledLight = config.relay_led_light * 10 + 23;
	}
	if (server->hasArg(F("relay_led_time")))
	{
		config.relay_led_time = server->arg(F("relay_led_time")).toInt();
		if (config.relay_led_type == 2 && ledTicker->active())
		{
			ledTicker->detach();
		}
	}
	checkCanLed(true);
	Config::saveConfig();

	server->send(200, F("text/html"), F("{\"code\":1,\"msg\":\"已经设置成功。\"}"));
}

Relay::Relay()
{
	if (config.relay_led_light == 0)
	{
		config.relay_led_light = 100;
	}
	if (config.relay_led_time == 0)
	{
		config.relay_led_time = 2;
	}
	ledLight = config.relay_led_light * 10 + 23;
	if (config.relay_led_type == 2)
	{
		ledTicker = new Ticker();
	}
	if (GPIO_PIN[GPIO_RFRECV] != 99)
	{
		radioReceive = new RadioReceive();
		radioReceive->init(GPIO_PIN[GPIO_RFRECV]);
	}
	Relay::channels = 0;
	btns = new RelayButton[4];
	for (uint8_t ch = 0; ch < 4; ch++)
	{
		if (GPIO_PIN[GPIO_REL1 + ch] == 99)
		{
			continue;
		}
		Relay::channels++;

		pinMode(GPIO_PIN[GPIO_REL1 + ch], OUTPUT); // 继电器
		if (GPIO_PIN[GPIO_LED1 + ch] != 99)
		{
			pinMode(GPIO_PIN[GPIO_LED1 + ch], OUTPUT); // LED
		}
		if (GPIO_PIN[GPIO_KEY1 + ch] != 99)
		{
			btns[ch].set(ch, GPIO_PIN[GPIO_KEY1 + ch]);
		}

		// 0:开关通电时断开  1 : 开关通电时闭合  2 : 开关通电时状态与断电前相反  3 : 开关通电时保持断电前状态
		if (config.relay_power_on_state == 2)
		{
			switchRelay(ch, !config.relay_last_state[ch], false); // 开关通电时状态与断电前相反
		}
		else if (config.relay_power_on_state == 3)
		{
			switchRelay(ch, config.relay_last_state[ch], false); // 开关通电时保持断电前状态
		}
		else
		{
			switchRelay(ch, config.relay_power_on_state == 1, false); // 开关通电时闭合
		}
	}

	checkCanLed(true);
}

void Relay::loop()
{
	for (size_t ch = 0; ch < Relay::channels; ch++)
	{
		if (GPIO_PIN[GPIO_KEY1 + ch] != 99)
		{
			btns[ch].loop();
		}
	}
	if (radioReceive)
	{
		radioReceive->loop();
	}
}

void Relay::mqttDiscovery(boolean isEnable)
{
	if (!mqtt)
	{
		return;
	}
	char topic[50];
	char message[500];

	String tmp = mqtt->getCmndTopic(F("POWER"));
	for (size_t ch = 0; ch < Relay::channels; ch++)
	{
		sprintf(topic, "%s/light/%s_%d/config", config.mqtt_discovery_prefix, UID, (ch + 1));
		if (isEnable)
		{
			sprintf(message, HASS_DISCOVER_RELAY, UID, (ch + 1),
					Relay::channels == 1 ? tmp.c_str() : (tmp + (ch + 1)).c_str(),
					Relay::channels == 1 ? powerTopic.c_str() : (powerTopic + (ch + 1)).c_str(),
					mqtt->getTeleTopic(F("availability")).c_str());
			Debug.AddLog(LOG_LEVEL_INFO, PSTR("discovery: %s - %s"), topic, message);
			mqtt->publish(topic, message);
		}
		else
		{
			mqtt->publish(topic, "");
		}
	}
}

void Relay::mqttConnected()
{
	powerTopic = mqtt->getStatTopic(F("POWER"));
	if (config.mqtt_discovery)
	{
		mqttDiscovery(true);
		mqtt->doReport();
	}
}

void Relay::perSecondDo()
{
	if (perSecond % 60 != 0)
	{
		return;
	}
	checkCanLed();
}

void Relay::httpAdd(ESP8266WebServer *server)
{
	server->on(F("/relay_do"), std::bind(&Relay::handleRelaySet, this, server));
	server->on(F("/rf_do"), std::bind(&Relay::handleRadioReceive, this, server));
	server->on(F("/relay_setting"), std::bind(&Relay::handleRelaySetting, this, server));
}

String Relay::httpGetStatus(ESP8266WebServer *server)
{
	String data;
	for (size_t ch = 0; ch < channels; ch++)
	{
		data += ",\"relay_" + String(ch + 1) + "\":";
		data += lastState[ch] ? 1 : 0;
	}
	return data.substring(1);
}

void Relay::httpHtml(ESP8266WebServer *server)
{
	String radioJs = F("<script type='text/javascript'>");
	radioJs += F("function setDataSub(data,key){if(key.substr(0,5)=='relay'){var t=id(key);var v=data[key];t.setAttribute('class',v==1?'btn-success':'btn-info');t.innerHTML=v==1?'开':'关';return true}return false}");
	String page = F("<table class='gridtable'><thead><tr><th colspan='2'>开关状态</th></tr></thead><tbody>");
	page += F("<tr colspan='2' style='text-align:center'><td>");
	for (size_t ch = 0; ch < channels; ch++)
	{
		page += F(" <button type='button' style='width:50px' onclick=\"ajaxPost('/relay_do', 'do=T&c={ch}');\" id='relay_{ch}' ");
		page.replace(F("{ch}"), String(ch + 1));
		if (lastState[ch])
		{
			page += F("class='btn-success'>开</button>");
		}
		else
		{
			page += F("class='btn-info'>关</button>");
		}
	}
	page += F("</td></tr></tbody></table>");

	page += F("<form method='post' action='/relay_setting' onsubmit='postform(this);return false'>");
	page += F("<table class='gridtable'><thead><tr><th colspan='2'>开关设置</th></tr></thead><tbody>");
	page += F("<tr><td>上电状态</td><td>");
	page += F("<label class='bui-radios-label'><input type='radio' name='power_on_state' value='0'/><i class='bui-radios'></i> 开关通电时断开</label><br/>");
	page += F("<label class='bui-radios-label'><input type='radio' name='power_on_state' value='1'/><i class='bui-radios'></i> 开关通电时闭合</label><br/>");
	page += F("<label class='bui-radios-label'><input type='radio' name='power_on_state' value='2'/><i class='bui-radios'></i> 开关通电时状态与断电前相反</label><br/>");
	page += F("<label class='bui-radios-label'><input type='radio' name='power_on_state' value='3'/><i class='bui-radios'></i> 开关通电时保持断电前状态</label>");
	page += F("</td></tr>");
	radioJs += F("setRadioValue('power_on_state', '{v}');");
	radioJs.replace(F("{v}"), String(config.relay_power_on_state));

	if (GPIO_PIN[GPIO_LED1] != 99)
	{
		page += F("<tr><td>面板指示灯</td><td>");
		page += F("<label class='bui-radios-label'><input type='radio' name='led_type' value='0'/><i class='bui-radios'></i> 无</label>&nbsp;&nbsp;&nbsp;&nbsp;");
		page += F("<label class='bui-radios-label'><input type='radio' name='led_type' value='1'/><i class='bui-radios'></i> 普通</label>&nbsp;&nbsp;&nbsp;&nbsp;");
		page += F("<label class='bui-radios-label'><input type='radio' name='led_type' value='2'/><i class='bui-radios'></i> 呼吸灯</label>&nbsp;&nbsp;&nbsp;&nbsp;");
		//page += F("<label class='bui-radios-label'><input type='radio' name='led_type' value='3'/><i class='bui-radios'></i> WS2812</label>");
		page += F("</td></tr>");
		radioJs += F("setRadioValue('led_type', '{v}');");
		radioJs.replace(F("{v}"), String(config.relay_led_type));

		page += F("<tr><td>指示灯亮度</td><td><input type='range' min='1' max='100' name='led_light' value='{led_light}' onchange='ledLightRangOnChange(this)'/>&nbsp;<span>{led_light}%</span></td></tr>");
		page.replace("{led_light}", String(config.relay_led_light));
		radioJs += F("function ledLightRangOnChange(the){the.nextSibling.nextSibling.innerHTML=the.value+'%'};");

		page += F("<tr><td>渐变时间</td><td><input type='number' name='relay_led_time' value='{v}'>毫秒</td></tr>");
		page.replace(F("{v}"), String(config.relay_led_time));

		String tmp = "";
		for (uint8_t i = 0; i <= 23; i++)
		{
			tmp += F("<option value='{v1}'>{v}:00</option>");
			tmp += F("<option value='{v2}'>{v}:30</option>");
			tmp.replace(F("{v1}"), String(i * 100));
			tmp.replace(F("{v2}"), String(i * 100 + 30));
			tmp.replace(F("{v}"), i < 10 ? "0" + String(i) : String(i));
		}

		page += F("<tr><td>指示灯时间段</td><td>");
		page += F("<select id='led_start' name='led_start'>");
		page += tmp;
		page += F("</select>");
		page += F("&nbsp;&nbsp;到&nbsp;&nbsp;");
		page += F("<select id='led_end' name='led_end'>");
		page += tmp;
		page += F("</select>");

		radioJs += F("id('led_start').value={v1};");
		radioJs += F("id('led_end').value={v2};");
		radioJs.replace(F("{v1}"), String(config.relay_led_start));
		radioJs.replace(F("{v2}"), String(config.relay_led_end));
		page += F("</td></tr>");
	}
	if (radioReceive)
	{
		page += F("<tr><td>清空射频</td><td>");
		for (size_t ch = 0; ch < channels; ch++)
		{
			page += F(" <button type='button' style='width:60px' onclick=\"javascript:if(confirm('确定要清空射频遥控？')){ajaxPost('/rf_do', 'do=c&c={ch}');}\" class='btn-danger'>{ch}路</button>");
			page.replace(F("{ch}"), String(ch + 1));
		}

		page += F(" <button type='button' style='width:50px' onclick=\"javascript:if(confirm('确定要清空全部射频遥控？')){ajaxPost('/rf_do', 'do=c&c=0');}\" class='btn-danger'>全部</button>");
		page += F("</td></tr>");
	}

	page += F("<tr><td colspan='2'><button type='submit' class='btn-info'>设置</button></td></tr>");

	page += F("</tbody></table></form>");
	radioJs += F("</script>");

	server->sendContent(page);
	server->sendContent(radioJs);
}

bool Relay::moduleLed()
{
	if (radioReceive && radioReceive->studyCH > 0)
	{
		return true;
	}
	return false;
}

#endif