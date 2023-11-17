#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <ModbusBridgeWiFi.h>
#include <ModbusClientRTU.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "pages.h"

AsyncWebServer webServer(80);
Config config;
Preferences prefs;
ModbusClientRTU *MBclient;
ModbusBridgeWiFi MBbridge;
WiFiManager wm;

uint32_t modbusPollingInterval = 0;
uint32_t modbusPollingSlaveId = 1;

#define FIRST_REGISTER 0x0000
#define NUM_VALUES 50

bool mdata_ready = false;
bool ddata_ready = false;
uint16_t mstart = 0;
uint16_t mlength = 0;
uint16_t dstart = 0;
uint16_t dlength = 0;
uint8_t mvalues[NUM_VALUES];
uint16_t dvalues[NUM_VALUES];
uint32_t mrequest_time;
uint32_t drequest_time;

const char* serverName = "http://192.168.1.126:3000/telemetries";

String macStr;

// Define an onData handler function to receive the regular responses
// Arguments are received response message and the request's token
void handleData(ModbusMessage response, uint32_t token) 
{
  // First value is on pos 3, after server ID, function code and length byte
  uint16_t offs = 3;

  if (response.getFunctionCode() == READ_COIL) { // FC1 Read Coils
    dbgln("Response Read Coil");
    uint32_t l = mlength / 8;
    if (mlength % 8 > 0) l++;

    for (uint8_t i = 0; i < l; ++i) {
      uint8_t value;
      offs = response.get(offs, value);
      for (uint8_t j = 0; j < 8; j++) {
        mvalues[i * 8 + j] = value & 1;
        value = value >> 1;
      }
    }
    mdata_ready = true;
    mrequest_time = token;
  }
  
  if (response.getFunctionCode() == READ_HOLD_REGISTER) { //FC3 Read Holding Registers
    dbgln("Response Read Holding Register");
    for (uint8_t i = 0; i < dlength; ++i) {
      offs = response.get(offs, dvalues[i]);
    }
    ddata_ready = true;
    drequest_time = token;
  }

  // HEXDUMP_N("Data", response.data(), response.size());
}

// Define an onError handler function to receive error responses
// Arguments are the error code returned and a user-supplied token to identify the causing request
void handleError(Error error, uint32_t token) 
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  LOG_E("Error response: %02X - %s\n", (int)me, (const char *)me);
}

String macToString(const uint8_t* mac) {
  char buf[19]; // 18 characters for MAC and one for null terminator
  snprintf(buf, sizeof(buf), "%03d%03d%03d%03d%03d%03d", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

void setup() {
  debugSerial.begin(115200);
  dbgln();
  dbgln("[config] load")
  prefs.begin("modbusRtuGw");
  config.begin(&prefs);
  debugSerial.end();
  debugSerial.begin(config.getSerialBaudRate(), config.getSerialConfig());
  dbgln("[wifi] start");
  WiFi.mode(WIFI_STA);
  wm.setClass("invert");
  auto reboot = false;
  wm.setAPCallback([&reboot](WiFiManager *wifiManager){reboot = true;});
  wm.autoConnect();
  if (reboot){
    ESP.restart();
  }
  dbgln("[wifi] finished");

  // Get MAC address
  uint8_t baseMac[6];
  WiFi.macAddress(baseMac);

  // Convert MAC address to String
  macStr = macToString(baseMac);
  Serial.print("SN : ");
  Serial.println(macStr);

  dbgln("[modbus] start");

  MBUlogLvl = LOG_LEVEL_WARNING;
  RTUutils::prepareHardwareSerial(modbusSerial);
#if defined(RX_PIN) && defined(TX_PIN)
  // use rx and tx-pins if defined in platformio.ini
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig(), RX_PIN, TX_PIN );
  dbgln("Use user defined RX/TX pins");
#else
  // otherwise use default pins for hardware-serial2
  modbusSerial.begin(config.getModbusBaudRate(), config.getModbusConfig());
#endif

  MBclient = new ModbusClientRTU(config.getModbusRtsPin());
  // Set up ModbusRTU client.
  // - provide onData handler function
  MBclient->onDataHandler(&handleData);
  // - provide onError handler function
  MBclient->onErrorHandler(&handleError);
  MBclient->setTimeout(2000);
  MBclient->begin(modbusSerial, 1);
  for (uint8_t i = 1; i < 248; i++)
  {
    MBbridge.attachServer(i, i, ANY_FUNCTION_CODE, MBclient);
  }  
  MBbridge.start(config.getTcpPort(), 10, config.getTcpTimeout());
  dbgln("[modbus] finished");
  setupPages(&webServer, MBclient, &MBbridge, &config, &wm);
  webServer.begin();
  dbgln("[setup] finished");

  MBclient->useModbusRTU();

  modbusPollingInterval = config.getPollingInterval();
  modbusPollingSlaveId = config.getPollingSlaveId();
  mstart = config.getPollingMstart();
  mlength = config.getPollingMlength();
  dstart = config.getPollingDstart();
  dlength = config.getPollingDlength();
  Serial.printf("M: %04X -> %d\n", mstart, mlength);
  Serial.printf("D: %04X -> %d\n", dstart, dlength);

  // uint8_t cData[] = { 0xFF, 0x00, 0xFF, 0x00};

  // Error err = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, WRITE_MULT_COILS, mstart, 32, 4, cData);
  // if (err!=SUCCESS) {
  //   ModbusError e(err);
  //   Serial.printf("Error #1 creating request: %02X - %s\n", (int)e, (const char *)e);
  // }

  // uint16_t wData[] = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666 };

  // err = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, WRITE_MULT_REGISTERS, dstart, 6, 12, wData);
  // if (err!=SUCCESS) {
  //   ModbusError e(err);
  //   Serial.printf("Error #2 creating request: %02X - %s\n", (int)e, (const char *)e);
  // }

  mdata_ready = false;
  // Issue the request
  dbgln("Request Read Coil");
  Error merr = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, READ_COIL, mstart, mlength);
  if (merr!=SUCCESS) {
    ModbusError e(merr);
    LOG_E("Error creating request: %02X - %s\n", (int)e, (const char *)e);
  }

  ddata_ready = false;
  // Issue the request
  dbgln("Request Read Holding Register");
  Error derr = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, READ_HOLD_REGISTER, dstart, dlength);
  if (derr!=SUCCESS) {
    ModbusError e(derr);
    LOG_E("Error creating request: %02X - %s\n", (int)e, (const char *)e);
  }

}

void loop() {
  static unsigned long next_request = millis();

  if (modbusPollingInterval >= 1000) {
      mstart = config.getPollingMstart();
      mlength = config.getPollingMlength();
      dstart = config.getPollingDstart();
      dlength = config.getPollingDlength();

    // Shall we do another request?
    if (millis() - next_request > modbusPollingInterval) {

      if (!mdata_ready){
        // Issue the request
        Error err = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, READ_COIL, mstart & 0xFFFF, mlength & 0xFFFF);
        if (err!=SUCCESS) {
          ModbusError e(err);
          LOG_E("Error read coil request: %02X - %s\n", (int)e, (const char *)e);
          mdata_ready = false;
        }
      }
      
      if (!ddata_ready) {
        // Issue the request
        Error err = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, READ_HOLD_REGISTER, dstart & 0xFFFF, dlength & 0xFFFF);
        if (err!=SUCCESS) {
          ModbusError e(err);
          LOG_E("Error read holding register request: %02X - %s\n", (int)e, (const char *)e);
          ddata_ready = false;
        }
      }
      
      // Save current time to check for next cycle
      next_request = millis();
    } else {
      // No, but we may have another response
      if (mdata_ready) {
        // We do. Print out the data
        debugSerial.printf("Requested at %3.3fs:\n", mrequest_time / 1000.0);

        // Create a JSON object to hold the data
        StaticJsonDocument<512> jsonData;
        static char addr[10];
        for (uint8_t i = 0; i < mlength; ++i) {
          // debugSerial.printf("   M%04d: %d\n", i + mstart, mvalues[i]);
          sprintf(addr, "M%d", i + mstart);
          jsonData[addr] = mvalues[i];
        }

        String data;
        serializeJson(jsonData, data);

        StaticJsonDocument<1024> jsonTelemetry;
        // jsonTelemetry["device_id"] = "c25e8f66-9445-48cd-98a4-4f953dde252c";
        jsonTelemetry["device_sn"] = macStr;
        jsonTelemetry["type"] = "json";
        jsonTelemetry["payload"] = data;

        String telemetries;
        serializeJson(jsonTelemetry, telemetries);

        HTTPClient http;

        // Your Domain name with URL path or IP address with path
        http.begin(serverName);

        // Specify content-type header
        http.addHeader("Content-Type", "application/json");
        
        // Send HTTP POST request
        int httpResponseCode = http.POST(telemetries);

        if (httpResponseCode > 0) {
          String response = http.getString(); // Get the response to the request
          debugSerial.println(httpResponseCode);   // Print return code
          debugSerial.println(response);           // Print request answer
        } else {
          debugSerial.print("Error on sending POST: ");
          debugSerial.println(httpResponseCode);
        }

        // Free resources
        http.end();
        jsonData.clear();
        jsonTelemetry.clear();

        debugSerial.printf("----------\n\n");
        mdata_ready = false;
      }

      if (ddata_ready) {
        // We do. Print out the data
        debugSerial.printf("Requested at %3.3fs:\n", drequest_time / 1000.0);

        // Create a JSON object to hold the data
        StaticJsonDocument<1024> jsonData;
        static char addr[10];
        for (uint8_t i = 0; i < dlength; ++i) {
          // debugSerial.printf("   D%04d: %d\n", i + dstart, dvalues[i]);
          sprintf(addr, "D%d", i + dstart);
          jsonData[addr] = dvalues[i];
        }

        String data;
        serializeJson(jsonData, data);

        StaticJsonDocument<2048> jsonTelemetry;
        // jsonTelemetry["device_id"] = "c25e8f66-9445-48cd-98a4-4f953dde252c";
        jsonTelemetry["device_sn"] = macStr;
        jsonTelemetry["type"] = "json";
        jsonTelemetry["payload"] = data;

        String telemetries;
        serializeJson(jsonTelemetry, telemetries);

        HTTPClient http;

        // Your Domain name with URL path or IP address with path
        http.begin(serverName);

        // Specify content-type header
        http.addHeader("Content-Type", "application/json");
        
        // Send HTTP POST request
        int httpResponseCode = http.POST(telemetries);

        if (httpResponseCode > 0) {
          String response = http.getString(); // Get the response to the request
          debugSerial.println(httpResponseCode);   // Print return code
          debugSerial.println(response);           // Print request answer
        } else {
          debugSerial.print("Error on sending POST: ");
          debugSerial.println(httpResponseCode);
        }

        // Free resources
        http.end();
        jsonData.clear();
        jsonTelemetry.clear();

        debugSerial.printf("----------\n\n");
        ddata_ready = false;
      }
    }
  }
  // put your main code here, to run repeatedly:
}