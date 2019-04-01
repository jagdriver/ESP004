// -------------------------------
// ESP004 Temp sensor Sketch
//
// This sketch is based on ESPBasic.ino
//
// Niels J. Nielsen
// WaveSnake 2018-01-19
// Moved from Visual Studio Community / VisualMicro to
// Visual Studio Code / Arduino 2019-03-31
// -------------------------------
#include <errno.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESP.h>
#include <WSESPConfig.h>
#include <WSESPTime.h>
#include <SimpleDHT.h>
#include <ArduinoOTA.h>
#include <ESP8266httpUpdate.h>

#define SKETCH_NAME "ESP004"
#define SKETCH_BIN "ESP004.bin"
#define SKETCH_VERSION "2.1.1"

extern "C"
{
#include "user_interface.h";
}

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
//enum msgCommand { UPDATE = 0, RESET = 1, SLEEP = 2, CONFIG = 3, STATE = 4, CLOSE = 5, OPEN = 6, OFF = 7, ON = 8, TEST = 9, START = 10, STOP = 11 } cmd;
//enum msgComponent { TEMP = 0, TEMPHUM = 1, GATE = 2, RELAY = 3, GASRELAY = 4, SWITCH = 5, BURNER = 6 };

enum Commands {DATA = 0, STATE = 1, UPDATE = 2, CONFIG = 3, ACTION = 4 };
String commands[1][20] = { "DATA", "STATE", "UPDATE", "CONFIG", "ACTION" };

enum SubCommand { UPDATECONFIG = 0, READCONFIG = 1, SENDDATA = 2, SENDSTATE = 3, UPDATESKETCH = 4, RESET = 5, UPDATETIMER = 6, START = 7, STOP = 8, ON = 9, OFF = 10, OPEN = 11, CLOSE = 12, SLEEP = 13, WAKEUP = 14};
String subCommands[1][20] = { "UPDATECONFIG", "READCONFIG", "SENDDATA", "SENDSTATE", "UPDATESKETCH", "RESET", "UPDATETIMER", "START", "STOP", "ON", "OFF", "OPEN", "CLOSE", "SLEEP", "WAKEUP"};

// DHT22 Temp sensor
SimpleDHT22 dht22;
int pinDHT22 = 5;

//float max_temp = 25;
//float min_temp = 20;
//float set_temp = 22;

#pragma region OS Timer handling

void timer_init(void)
{
	// NTP Time Update timer
	String osTimerNTP = cnfg->GetConfigString("ostimerntp");
	os_timer_setfn(&firstTimer, firstCallback, NULL);
	//os_timer_arm(&firstTimer, atoi(osTimerNTP.c_str()), true);
	os_timer_arm(&firstTimer, 60000, true);

	// NTP Sync timer
	String osTimerNTPSync = cnfg->GetConfigString("ostimerntpsync");
	os_timer_setfn(&secondTimer, secondCallback, NULL);
	//os_timer_arm(&secondTimer, atoi(osTimerNTPSync.c_str()), true);
	os_timer_arm(&secondTimer, 1000, true);

	// Temperature measurement timer
	String osTimerMeasurement = cnfg->GetConfigString("ostimermeasurement");
	os_timer_setfn(&thirdTimer, thirdCallback, NULL);
	os_timer_arm(&thirdTimer, atoi(osTimerMeasurement.c_str()), true);
	//os_timer_arm(&thirdTimer, 30000, true);
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
	String sTopic = cnfg->GetConfigString("account") + "/" +
		cnfg->GetConfigString("accessorytype") + "/" + cnfg->GetConfigString("accessoryid");

	String sPayload = temperaturePayload(
		temperature,
		humidity,
		strtod(cnfg->GetConfigString("accessorymintemp").c_str(), 0),
		strtod(cnfg->GetConfigString("accessorymaxtemp").c_str(), 0));

	
	if (mqttClient.publish(sTopic.c_str(), sPayload.c_str()))
	{
		
		Serial.println("\nPublishing temp/hum: " + String(temperature) + " - " + String(humidity) + " - " + ntpTime.GetTimeString());
	}

	//sTopic = cnfg->GetConfigString("license") + "/" +
	//	cnfg->GetConfigString("accessoryhomeid") + "/" +
	//	cnfg->GetConfigString("accessoryroomid") + "/" +
	//	cnfg->GetConfigString("accessoryid") + "/configuration";

	// Here we must send the data and then decide if we also should sen a command
	// to inform about the state
	//
	sTopic = cnfg->GetConfigString("account") + "/" +
		"command" + "/" + "98B4876540B7202E"; 
	// (Gas Burnet AccessoryId)

	if (temperature > strtod(cnfg->GetConfigString("accessorymaxtemp").c_str(), 0)) //set_temp)
	{
		// stop gas burner
		sPayload = commandPayload("stop");
		mqttClient.publish(sTopic.c_str(), sPayload.c_str());
		
	}

	if (temperature < strtod(cnfg->GetConfigString("accessorymintemp").c_str(), 0)) //set_temp)
	{
		// Start gas burner
		sPayload = commandPayload("start");
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
	ESPhttpUpdate.update(cnfg->GetConfigString("sketchserver"),cnfg->GetConfigStringAsUint16("sketchserverport"), cnfg->GetConfigString("sketchbin"));

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

#pragma region WiFi Callback
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

int getCommand(String command)
{
	for (int i = 0; i < 6; i++)
	{
		command.toUpperCase();
		if (command.equals(commands[0][i]))
		{
			return i;
		}
	}
	return 0;
}

int getSubCommand(String command)
{
	for (int i = 0; i < 6; i++)
	{
		command.toUpperCase();
		if (command.equals(subCommands[0][i]))
		{
			return i;
		}
	}
	return 0;
}

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
	
	String accessoryId = root.get<String>("accessoryid");
	String messageType = root.get<String>("messagetype");
	int cmd = getCommand(root.get<String>("messagetype"));

	//Serial.printf("\nAccessoryId received: %s", accessoryId.c_str());
	//Serial.printf("\nAccessoryType received: %s", messageType.c_str());

	//int numberOfElements = root["data"].size();
	//Serial.printf("\nNumber of data elements: %i", numberOfElements);

	JsonObject& commandElement = root["data"][0];
	JsonObject& dataElement = root["data"][1];

	String commandName = commandElement["name"];
	String commandValue = commandElement["value"];
	int subcmd = getSubCommand(commandElement["name"]);

	String dataName = dataElement["name"];
	String dataValue = dataElement["value"];

	//Serial.printf("\nCommandName received: %s", commandName.c_str());
	//Serial.printf("\nCommandValue received: %s", commandValue.c_str());

	//String dataName = dataElement["name"];
	//String dataValue = dataElement["value"];

	//Serial.printf("\nDataName received: %s", dataName.c_str());
	//Serial.printf("\nDataValue received: %s", dataValue.c_str());

	//Serial.println("\nCommand array length: " + (sizeof(commands) / sizeof(*commands)));
	// This will be a string
	// So we have to parse the jsonStructure

	//Serial.printf("\nCommand received: %i", cmd);
	//Serial.printf("\nSubCommand received: %i", subcmd);

	switch (cmd)
	{
	case DATA:
		Serial.println("\nDATA Command received");
		switch (subcmd)
		{
		case SENDDATA:
			// Here we send tha data
			Serial.println("\nSendData Command received");
			break;
		default:
			break;
		}
		break;
	case STATE:
		Serial.println("\nState Command received");
		switch (subcmd)
		{
		case SENDSTATE:
			// Here we send the current state 
			Serial.println("\nSendState Command received");
			break;
		default:
			break;
		}
		break;
	case UPDATE:
		Serial.println("\nUpdate Commend received");
		switch (subcmd)
		{
		case UPDATECONFIG:
			// Here we update the sketch
			Serial.println("\nUPDATESKETCH Command received");
			//ota_Update();
			break;
		case UPDATETIMER:
			// Here we update the Timer values
			Serial.println("\nUPDATETIMER Command received");
			break;
		default:
			break;
		}
		break;
	case CONFIG:
		Serial.println("\nCONFIG Command received");
		switch (subcmd)
		{
		case UPDATECONFIG:
			// Here we update the config 
			cnfg->SetConfigString(dataName,dataValue);
			cnfg->SaveConfig("/ESP01.txt");
			break;
		case READCONFIG:
			// Here we read and send the config 
			Serial.println("\nREADCONFIG Command.......");
			break;
		}
		break;
	case ACTION:
		Serial.println("\nAction Command receivd");
		switch (subcmd)
		{
		case START:
			// Here we start the measurement 
			Serial.println("\nSTART Command received");
			break;
		case STOP:
			// Here we stop the measurement
			Serial.println("\nSTOP Command received");
			break;
		case RESET:
			// Here we reset the ESP
			Serial.println("\nSTOP Command received");
			break;
		case SLEEP:
			// Here we go to sleep
			Serial.println("\nSTOP Command received");
			break;
		case WAKEUP:
			// Here we wakeup from sleep
			Serial.println("\nSTOP Command received");
			break;
		default:
			break;
		}
		break;
	default:
		break;
	}
}

void mqttConnect()
{
	while (!mqttClient.connected())
	{
		if (mqttClient.connect(cnfg->GetConfigChar("accessoryid"), cnfg->GetConfigChar("mqttusername"), cnfg->GetConfigChar("mqttpass")))
		{
			Serial.println("\nMQTT Client Connected");
			mqttSubscribe();
		}
		else
		{
			// Wait 5 seconds before retrying
			Serial.println("\n!Mqtt Client not connected");
			delay(2000);
		}
	}
}

char * temperaturePayload(float temperature, float humidity, float mintemp,	float maxtemp)
{
	//
	// Create Json Object as the MQTT Temperature Payload.
	// It's easy to parse for the receiving party.
	// We take most of the parameters from the
	// Config structure.
	// The buffer are 500 bytes
	//
	char buff[10];
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();
	JsonArray& Data = root.createNestedArray("data");
	root["accessoryid"] = cnfg->GetConfigString("accessoryid");
	root["accessoryname"] = cnfg->GetConfigString("accessoryname");
	root["accountid"] = cnfg->GetConfigString("account");
	root["sketchname"] = cnfg->GetConfigString("sketchname");
	root["accessorytype"] = cnfg->GetConfigString("accessorytype");
	root["messagetype"] = "data";

	// Here we test for message type and
	// generate relevant entries
	JsonObject& Data1 = Data.createNestedObject();


	Data1["name"] = "Current";
	dtostrf(temperature, 5, 1, buff);
	Data1["value"] = String(buff);

	JsonObject& Data2 = Data.createNestedObject();
	Data2["name"] = "Humidity";
	dtostrf(humidity, 5, 1, buff);
	Data2["value"] = String(buff);

	JsonObject& Data3 = Data.createNestedObject();
	Data3["name"] = "MIN";
	dtostrf(mintemp, 5, 1, buff);
	Data3["value"] = String(buff);

	JsonObject& Data4 = Data.createNestedObject();
	Data4["name"] = "MAX";
	dtostrf(maxtemp, 5, 1, buff);
	Data4["value"] = String(buff);


	// Print Json object to char array
	char jsonChar[MQTT_BUFFER_SIZE];
	root.printTo((char*)jsonChar, root.measureLength() + 1);
	
	return jsonChar;
}

char * commandPayload(String command)
{
	//
	// Create Json Object as the MQTT Temperature Payload.
	// It's easy to parse for the receiving party.
	// We take most of the parameters from the
	// Config structure.
	// The buffer are 500 bytes
	//
	char buff[10];
	DynamicJsonBuffer jsonBuffer;
	JsonObject& root = jsonBuffer.createObject();
	JsonArray& Data = root.createNestedArray("data");
	root["accessoryid"] = cnfg->GetConfigString("accessoryid");
	root["accountid"] = cnfg->GetConfigString("account");
	root["sketchname"] = cnfg->GetConfigString("sketchname");
	root["accessorytype"] = cnfg->GetConfigString("accessorytype");
	root["messagetype"] = "data";

	// Here we test for message type and
	// generate relevant entries

	JsonObject& Data1 = Data.createNestedObject();
	Data1["name"] = "Command";
	Data1["value"] = "stop";

	// Print Json object to char array
	char jsonChar[MQTT_BUFFER_SIZE];
	root.printTo((char*)jsonChar, root.measureLength() + 1);

	return jsonChar;
}

void mqttPublish()
{
	// 
	// Publish a MQTT message, one with a simple string payload
	// and one with a Json object as payload
	//
	//String sTopic = cnfg->GetConfigString("license") + "/" +
	//	cnfg->GetConfigString("accessoryhomeid") + "/" +
	//	cnfg->GetConfigString("accessoryroomid") + "/" +
	//	cnfg->GetConfigString("accessoryid");

	//// Publist TEST Message
	//mqttClient.publish(sTopic.c_str(), "{\"command\":\"9\"}"); // 9 = TEST
	//Serial.printf("\nMQTT Published to %s", sTopic.c_str());
}

void mqttSubscribe()
{
	//
	// Here you subscribe to Config messages
	//
	String sTopic = cnfg->GetConfigString("account") + "/" +
		//cnfg->GetConfigString("accessorytype") 
		"command" + "/" +
		cnfg->GetConfigString("accessoryid");

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

// String  var = getValue( StringVar, ',', 2); // if  a,4,D,r  would return D        
String getSubString(String data, char separator, int index)
{
	int found = 0;
	int strIndex[] = { 0, -1 };
	int maxIndex = data.length();

	for (int i = 0; i <= maxIndex && found <= index; i++) {
		if (data.charAt(i) == separator || i == maxIndex) {
			found++;
			strIndex[0] = strIndex[1] + 1;
			strIndex[1] = (i == maxIndex) ? i + 1 : i;
		}
	}
	return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
} 

IPAddress GetIPAddress(String name)
{
	String addressStr = cnfg->GetConfigString(name);
	String byte1 = getSubString(addressStr,'.',0);
	String byte2 = getSubString(addressStr, '.', 1);
	String byte3 = getSubString(addressStr, '.', 2);
	String byte4 = getSubString(addressStr, '.', 3);
	IPAddress address = { atoi(byte1.c_str()), atoi(byte2.c_str()), atoi(byte3.c_str()), atoi(byte4.c_str()) };
	return address;
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
	Serial.println("Sketch: " + cnfg->GetConfigString("sketchname") + " Version: " + cnfg->GetConfigString("sketchversion"));

	// --- Local in memory Configuration structure Initialization ---
	WSESPConfig config("/ESP01.txt");
	cnfg = &config;

	// --- Set Sketch properties in local config
	cnfg->SetConfigString("sketchname", String(SKETCH_NAME));
	cnfg->SetConfigString("sketchbin", String(SKETCH_BIN));
	cnfg->SetConfigString("sketchversion", String(SKETCH_VERSION));

	// If below line is uncommented, a list of all config values will be printed
	cnfg->PrintLocalConfig();
	Serial.print("\nConfiguration Loaded from SPIFFS");

	// --- WiFi Connect ---
	// The event handlers will tell us if and why we cannot connect
	e1 = WiFi.onStationModeGotIP(onSTAGotIP);
	e2 = WiFi.onStationModeDisconnected(onSTADisconnected);
	e3 = WiFi.onStationModeConnected(onSTAConnected);
	
	WiFi.disconnect();

	if (cnfg->GetConfigString("clientwificonfig").equals("static"))
	{
		WiFi.config(GetIPAddress("clientip"), GetIPAddress("clientgw"), GetIPAddress("clientsubnet"), GetIPAddress("clientdns1"), GetIPAddress("clientdns2"));
	}

	//WiFi.config(IPAddress(192,168,1,104), IPAddress(192,168,1,1), IPAddress(255,255,255,0), IPAddress(194,239,134,83), IPAddress(193,162,153,164));	
	WiFi.enableAP(false);
	WiFi.enableSTA(true);
	WiFi.begin(cnfg->GetConfigString("clientssid").c_str(), cnfg->GetConfigString("clientpass").c_str());

	while (WiFi.status() != WL_CONNECTED)
	{
		Serial.print(".");
		delay(200);
	}


	// MQTT Message Broker Client Setup 
	uint16_t port;
	if (str_to_uint16(cnfg->GetConfigChar("mqttserverport"), &port))
	{
		mqttClient.setServer(cnfg->GetConfigChar("mqttserver"), port);
		mqttClient.setCallback(mqttCallback);
	}
	

	// Test Publish and Subscribe...
	//mqttConnect();
	//mqttSubscribe();
	//mqttPublish();

	// REST Server connection verification
	// Cannot connect to rest server Ethernet adapter  !!!!!
	// Only WiFi adapter
	//
	//CallRestGet("http://" + cnfg->GetConfigString("restserver") + ":" + cnfg->GetConfigString("restserverport") + "/admin/log");
	CallRestGet(cnfg->GetConfigString("restserver") + ":" + cnfg->GetConfigString("restserverport") + "/admin/test");
	
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
		mqttConnect();
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
