// -------------------------------
// ESP004 Temp sensor Sketch
//
// This sketch is based on ESPBasic.ino
//
// Niels J. Nielsen
// WaveSnake 2018-01-19
// -------------------------------

#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>

#include <ArduinoJson\ArduinoJson.h>
#include <ESP.h>
#include <WSESPConfig\WSESPConfig.h>
#include <WSESPTime.h>
#include <SimpleDHT.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>

extern "C"
{
#include "user_interface.h";
}


// Sketch meta data
const char * SKETCH_DISPLAYNAME = "ESP004";
const char * SKETCH_BIN = "/ESP004.bin";
const char * SKETCH_VERSION = "1.3.4";
const char * SKETCH_SERVER = "192.168.1.207";

// OS Timers
os_timer_t firstTimer;
bool firstTickOcurred;
os_timer_t secondTimer;
bool secondTickOcurred;
os_timer_t thirdTimer;
bool thirdTickOcurred;
unsigned long ticsSinceNTPUpdate = 0;
unsigned long ticsAtNTPUpdate = 0;
unsigned long ticsAjustment = 3000;

// Configuration
WSESPConfig * cnfg;

// WiFi
WiFiClient wifiClient;
static WiFiEventHandler e1, e2, e3;
IPAddress timeServerIP;
IPAddress dnsServerIP;

// NTP
WSESPTime ntpTime;

// MQTT Client
PubSubClient mqttClient(wifiClient);
bool msgTest = false;
const int MQTT_BUFFER_SIZE = 1500;
enum msgCommand { UPDATE = 0, RESET = 1, SLEEP = 2, CONFIG = 3, STATE = 4, CLOSE = 5, OPEN = 6, OFF = 7, ON = 8, TEST = 9, START = 10, STOP = 11 } cmd;
enum msgComponent { TEMP = 0, TEMPHUM = 1, GATE = 2, RELAY = 3, GASRELAY = 4 };

// THT22 Temp sensor
SimpleDHT22 dht22;
int pinDHT22 = 5;
float max_temp = 25;
float min_temp = 20;
float set_temp = 22;

#pragma region OS Timer handling

void timer_init(void)
{
	// NTP Time Update timer
	os_timer_setfn(&firstTimer, firstCallback, NULL);
	os_timer_arm(&firstTimer, 600000, true);

	// NTP Sync timer
	os_timer_setfn(&secondTimer, secondCallback, NULL);
	os_timer_arm(&secondTimer, 60000, true);

	// Temperature measurement timer
	os_timer_setfn(&thirdTimer, thirdCallback, NULL);
	os_timer_arm(&thirdTimer, 30000, true);
}

void firstCallback(void *pArg)
{
	firstTickOcurred = true;
}

void secondCallback(void *pArg)
{
	secondTickOcurred = true;
}

void thirdCallback(void *pArg)
{
	thirdTickOcurred = true;
}

#pragma endregion

#pragma region NTP Time server handling

void ntp_Update()
{
	// Get NTP time server IP
	WiFi.hostByName(cnfg->GetConfigString("ntphost").c_str(), timeServerIP);

	// Get NTP Time 
	ntpTime.ReadNTPTime(cnfg->GetConfigString("ntphost").c_str(), timeServerIP);
	ticsAtNTPUpdate = millis();
}

void ntp_Sync(long ms)
{
	// Here we synchronize the NTP time with the elapsed CPU Ticks
	ntpTime.SyncTime(ms);
}
#pragma endregion

#pragma region Temperature measurement handling

void temp_Update()
{
	// Read temperature from DHT22
	float temperature = 0;
	float humidity = 0;
	String msg;
	int err = SimpleDHTErrSuccess;
	if ((err = dht22.read2(pinDHT22, &temperature, &humidity, NULL)) != SimpleDHTErrSuccess)
	{
		msg = "Read DHT22 failed";
	}
	else
	{
		msg = "Temperature: " + String(temperature) + " Humidity: " + String(humidity);
	}

	// Publish temperature to message broker
	String sTopic = cnfg->GetConfigString("license") + "/" +
		cnfg->GetConfigString("accessoryhomeid") + "/" +
		cnfg->GetConfigString("accessoryroomid") + "/" +
		cnfg->GetConfigString("accessoryid") + "/measurement";
	String sPayload = temperaturePayload(STATE, TEMP, "component001", temperature, humidity, set_temp, min_temp, max_temp, "ESP004 Temperatur");
	mqttClient.publish(sTopic.c_str(), sPayload.c_str());

	// Publich "Configuration" START or STOP message
	sTopic = cnfg->GetConfigString("license") + "/" +
		cnfg->GetConfigString("accessoryhomeid") + "/" +
		"ROOM002" + "/" +
		"ESP001" + "/configuration";

	//sTopic = cnfg->GetConfigString("license") + "/" +
	//	cnfg->GetConfigString("accessoryhomeid") + "/" +
	//	cnfg->GetConfigString("accessoryroomid") + "/" +
	//	cnfg->GetConfigString("accessoryid") + "/configuration";

	if (temperature > set_temp)
	{
		// stop gas burner
		sPayload = temperaturePayload(STOP, TEMP, "component001", temperature, humidity, set_temp, min_temp, max_temp, "ESP004 Stop Gasfyr");
		mqttClient.publish(sTopic.c_str(), sPayload.c_str());
	}
	if (temperature < set_temp)
	{
		// Start gas burner
		sPayload = temperaturePayload(START, TEMP, "component001", temperature, humidity, set_temp, min_temp, max_temp, "ESP004 Start Gasfyr");
		mqttClient.publish(sTopic.c_str(), sPayload.c_str());
	}
}

#pragma endregion

#pragma region Configuration handling

void config_Update()
{
	// Read configuration from REST server
}


#pragma endregion

#pragma region OTA software Update

void ota_Update()
{
	ESPhttpUpdate.update(String(SKETCH_SERVER), 80, String(SKETCH_BIN));

	if (ESPhttpUpdate.getLastError() == 0)
	{
		esp_reset();
	}
	else
	{
		Serial.println("Update error: " + ESPhttpUpdate.getLastErrorString());
	}
}

#pragma endregion

#pragma region WiFi Callback's
void onSTAGotIP(WiFiEventStationModeGotIP ipInfo)
{
	Serial.printf("\nGot IP: %s", ipInfo.ip.toString().c_str());
	Serial.printf("\nGot GW: %s", ipInfo.gw.toString().c_str());
	Serial.printf("\nGot MASK: %s", ipInfo.mask.toString().c_str());
}

void onSTADisconnected(WiFiEventStationModeDisconnected event_info)
{
	Serial.printf("\nDisconnected from SSID: %s", event_info.ssid.c_str());
	Serial.printf("\nReason: %d", event_info.reason);
}

void onSTAConnected(WiFiEventStationModeConnected event_info)
{
	Serial.printf("\nWiFi Connected to SSID %s", event_info.ssid.c_str());
}

#pragma endregion

#pragma region MessageBroker Handling

void mqttCallback(char* topic, byte* payload, unsigned int length)
{
	//
	// Here we receive MQTT messages
	// and parses the message into Json Object
	// We then call the the relevant function
	//

	StaticJsonBuffer<MQTT_BUFFER_SIZE> jsonBuffer;
	payload[length] = '\0';
	String s = String((char*)payload);
	JsonObject& root = jsonBuffer.parse(s);

	//Serial.printf("\nTopic received: %s", topic);
	//Serial.printf("\nPayload received: %s", s.c_str());
	
	String accessoryid = root.get<String>("accessoryid");

	int cmd = root.get<int>("command");
	//Serial.println("\nCommand received: " + cmd);

	switch (cmd)
	{
	case UPDATE:
		Serial.println("\nUpdating Sketch.....");
		ota_Update();
		break;
	case RESET:
		Serial.println("\nResetting Sketch........");
		ESP.reset();
		break;
	case SLEEP:
		Serial.println("\nPutting the Accessory into deep sleep........");
		break;
	case CONFIG:
		Serial.println("\nUpdate the Accessory configuration.......");
		//UpdateLocalConfig();  // Get new config from rest server
		break;
	case STATE:
		Serial.println("\nReport the current Component state........");
		break;
	case CLOSE:
		Serial.println("\nClose the Component relay........");
		break;
	case OPEN:
		Serial.println("\nOpen the Component relay........");
		break;
	case OFF:
		Serial.println("\nTurn off the component........");
		break;
	case ON:
		Serial.println("\nTurn on the component........");
		break;
	case TEST:
		msgTest = true;
		Serial.print("\nMQTT Publish Subscribe test OK");
		break;
	default:
		break;
	}
}

void mqttConnect(char *mqttserver, uint16_t port)
{
	mqttClient.setServer(mqttserver, port);
	mqttClient.setCallback(mqttCallback);

	while (!mqttClient.connected())
	{
		// Connect to MessageBroker
		if (mqttClient.connect(cnfg->GetConfigChar("accessoryid"), cnfg->GetConfigChar("owner"), cnfg->GetConfigChar("mqttpass")))
		{
			Serial.print("\nMQTT Client Connected");
		}
		else
		{
			// Wait 5 seconds before retrying
			Serial.printf("\n!Mqtt Client not connected state: %d", mqttClient.state());
			delay(5000);
		}
	}
}

char * accessoryPayload(int command, char * component)
{
	//
	// Create Json Object as the MQTT Payload.
	// It's easy to parse for the receiving party.
	// We take most of the parameters from the
	// Config structure.
	// "{84F894B5-3C2E-4445-A9A2-B61572856F1C}";
	//
	StaticJsonBuffer<MQTT_BUFFER_SIZE> jsonBuffer;
	char jsonChar[MQTT_BUFFER_SIZE];

	JsonObject& root = jsonBuffer.createObject();
	root["license"] = cnfg->GetConfigString("license");
	root["owner"] = cnfg->GetConfigString("owner");
	root["email"] = cnfg->GetConfigString("email");
	root["company"] = cnfg->GetConfigString("company");
	root["accessoryid"] = cnfg->GetConfigString("accessoryid");
	root["accessoryname"] = cnfg->GetConfigString("accessoryname");
	root["component"] = component;
	root["command"] = String(command);
	root["sketch"] = SKETCH_DISPLAYNAME;
	root["path"] = SKETCH_BIN;
	root["version"] = SKETCH_VERSION;
	root["message"] = "Log Message";

	// Print Json object to char array
	root.printTo((char*)jsonChar, root.measureLength() + 1);
	return jsonChar;
}

char * temperaturePayload(int command,
	int type,
	char * component,
	float temperature,
	float humidity,
	float settemp,
	float mintemp,
	float maxtemp,
	char * message)
{
	//
	// Create Json Object as the MQTT Temperature Payload.
	// It's easy to parse for the receiving party.
	// We take most of the parameters from the
	// Config structure.
	// The buffer are 500 bytes
	//
	StaticJsonBuffer<MQTT_BUFFER_SIZE> jsonBuffer;
	char jsonChar[MQTT_BUFFER_SIZE];

	JsonObject& root = jsonBuffer.createObject();
	root["license"] = cnfg->GetConfigString("license");
	root["owner"] = cnfg->GetConfigString("owner");
	root["email"] = cnfg->GetConfigString("email");
	root["company"] = cnfg->GetConfigString("company");
	root["accessoryid"] = cnfg->GetConfigString("accessoryid");
	root["sketchname"] = SKETCH_DISPLAYNAME;
	root["sketchversion"] = SKETCH_VERSION;
	root["component"] = component;
	root["componenttype"] = String(type);
	root["command"] = String(command);
	root["temperature"] = String(temperature);
	root["humidity"] = String(humidity);
	root["accessorymaxtemp"] = String(maxtemp);
	root["accessorymintemp"] = String(mintemp);
	root["accessorysettemp"] = String(settemp);
	root["utcdate"] = ntpTime.GetISODateString();
	root["message"] = message;

	// Print Json object to char array
	root.printTo((char*)jsonChar, root.measureLength() + 1);
	//Serial.printf("\nJsonChar: %s Length: %i", jsonChar, root.measureLength());
	return jsonChar;
}


void mqttPublish()
{
	// 
	// Publish a MQTT message, one with a simple string payload
	// and one with a Json object as payload
	//
	String sTopic = cnfg->GetConfigString("license") + "/" +
		cnfg->GetConfigString("accessoryhomeid") + "/" +
		cnfg->GetConfigString("accessoryroomid") + "/" +
		cnfg->GetConfigString("accessoryid");

	// Publist TEST Message
	mqttClient.publish(sTopic.c_str(), "{\"command\":\"9\"}"); // 9 = TEST
	Serial.printf("\nMQTT Published to %s", sTopic.c_str());
}

void mqttSubscribe()
{
	//
	// Here you subscribe to Config messages
	//
	String sTopic = cnfg->GetConfigString("license") + "/" +
		cnfg->GetConfigString("accessoryhomeid") + "/" +
		cnfg->GetConfigString("accessoryroomid") + "/" +
		cnfg->GetConfigString("accessoryid") + "/configuration";

	mqttClient.subscribe(sTopic.c_str());
	Serial.printf("\nMQTT Subscribed to %s", sTopic.c_str());
}

#pragma endregion

#pragma region REST Service handling

void CallRestGet(String url)
{
	if (WiFi.status() == WL_CONNECTED)
	{
		HTTPClient http;

		http.begin(url);
		int httpCode = http.GET();
		
		if (httpCode > 0)
		{
			String payload = http.getString();
			http.end();
			if (strstr(payload.c_str(), "received") != NULL)
			{
				Serial.print("\nRest Server Connected");
			}
			else
			{
				Serial.print("\nRest Server Not Connected!");
			}
		}
		else
		{
			http.end();
			Serial.printf("\nRest Server Not Connected! %d" + httpCode);
		}
	}
}

String CallRestPost(String url)
{
	if (WiFi.status() == WL_CONNECTED)
	{
		HTTPClient http;

		// Create and initialize a Json Object
		StaticJsonBuffer<300> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();
		root["license"] = "{84F894B5-3C2E-4445-A9A2-B61572856F1C}";
		root["owner"] = "Niels J. Nielsen";
		root["email"] = "jagdriver@hotmail.com";
		root["company"] = "WaveSnake";

		// Print Json object to String
		String jsonStr;
		root.printTo(jsonStr);

		http.begin(url);
		http.addHeader("Content-Type", "application/json");
		http.addHeader("charset", "utf-8");
		int httpCode = http.POST(jsonStr);

		String payload = http.getString();
		if (httpCode > 0)
		{
			http.end();
			return payload + " - " + httpCode;
		}
		http.end();  //Close connection
		return "No POST Response";
	}
}

String CallRestPut(String url)
{
	if (WiFi.status() == WL_CONNECTED)
	{
		HTTPClient http;

		// Create and initialize a Json Object
		StaticJsonBuffer<300> jsonBuffer;
		JsonObject& root = jsonBuffer.createObject();
		root["license"] = "{84F894B5-3C2E-4445-A9A2-B61572856F1C}";
		root["owner"] = "Niels J. Nielsen";
		root["email"] = "jagdriver@hotmail.com";
		root["company"] = "WaveSnake";

		// Print Json object to char array
		char jsonChar[300];
		root.printTo((char*)jsonChar, root.measureLength() + 1);

		http.begin(url);
		http.addHeader("Content-Type", "application/json");
		int httpCode = http.sendRequest("PUT", (uint8_t*)jsonChar, strlen(jsonChar));

		String payload = http.getString();
		if (httpCode > 0)
		{
			http.end();
			return payload + " - " + httpCode;

		}
		http.end();  //Close connection
	}
}

#pragma endregion

#pragma region Utility functions

bool str_to_uint16(const char *str, uint16_t *res) {
	char *end;
	errno = 0;
	long val = strtol(str, &end, 10);
	if (errno || end == str || *end != '\0' || val < 0 || val >= 0x10000) {
		return false;
	}
	*res = (uint16_t)val;
	return true;
}

void esp_reset()
{
	ESP.reset();
}

#pragma endregion

void setup()
{
	// --- Serial Connect ---
	Serial.begin(115200);
	while (!Serial);
	Serial.print("\nSerial Connected ");

	// Print Sketch name and version
	Serial.println("Sketch " + String(SKETCH_DISPLAYNAME) + " " + String(SKETCH_VERSION));

	// --- Local in memory Configuration structure Initialization ---
	WSESPConfig config("/ESP01.txt");
	cnfg = &config;

	// If below line is uncommented, a list of all config values will be printed
	cnfg->PrintLocalConfig();
	Serial.print("\nConfiguration Loaded from SPIFFS");

	// --- WiFi Connect ---
	// The event handlers will tell us if and why we cannot connect
	e1 = WiFi.onStationModeGotIP(onSTAGotIP);
	e2 = WiFi.onStationModeDisconnected(onSTADisconnected);
	e3 = WiFi.onStationModeConnected(onSTAConnected);

	WiFi.config(IPAddress(192,168,1,104), IPAddress(192,168,1,1), IPAddress(255,255,255,0), IPAddress(194,239,134,83), IPAddress(193,162,153,164));
	WiFi.enableAP(false);
	WiFi.enableSTA(true);
	WiFi.begin(cnfg->GetConfigString("clientssid").c_str(), cnfg->GetConfigString("clientpass").c_str());

	while (WiFi.status() != WL_CONNECTED)
	{
		Serial.print(".");
		delay(200);
	}

	// MQTT Message Broker Client Setup 
	mqttConnect(cnfg->GetConfigChar("mqttserver"), cnfg->GetConfigStringAsUint16("mqttserverport"));

	// Test Publish and Subscribe...
	mqttSubscribe();
	//mqttPublish();

	// REST Server connection verification
	// Cannot connect to rest server Ethernet adapter  !!!!!
	// Only WiFi adapter
	//
	//CallRestGet("http://" + cnfg->GetConfigString("restserver") + ":" + cnfg->GetConfigString("restserverport") + "/admin/log");
	CallRestGet("http://192.168.1.25:" + cnfg->GetConfigString("restserverport") + "/admin/log");
	
	// OS Timer Setup
	firstTickOcurred = false;
	secondTickOcurred = false;
	thirdTickOcurred = false;
	timer_init();

	// OTA Software Update Configuration
	ESPhttpUpdate.rebootOnUpdate(true);

	// NTP Initialization
	ntp_Update();
}

void loop()
{
	if (!mqttClient.connected())
	{
		mqttConnect(cnfg->GetConfigChar("mqttserver"), cnfg->GetConfigStringAsUint16("mqttserverport"));
	}
	mqttClient.loop();

	long now = millis();
	ticsSinceNTPUpdate = now - (ticsAtNTPUpdate - ticsAjustment);

	if (firstTickOcurred == true)
	{
		// It's time to renew NTP time
		ntp_Update();
		firstTickOcurred = false;
		ticsSinceNTPUpdate = 0;

		// Uncommebt to check for memory leak
		//uint32_t freeHeap = ESP.getFreeHeap();
		//Serial.printf("\nNTP Update, Free Heap %i", freeHeap);
	}

	if (secondTickOcurred == true)
	{
		// It's time to Sync NTP time with elapsed cpu tics 
		ntp_Sync(ticsSinceNTPUpdate);
		secondTickOcurred = false;
	}

	if (thirdTickOcurred == true)
	{
		// It's time to call the temperature measurement
		temp_Update();
		thirdTickOcurred = false;
	}
	delay(0);
	//
	// We have to check
	// ESP.deepSleep(200000, WAKE_NO_RFCAL);
	//
}
