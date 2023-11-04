#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Logging.h>
#include <ModbusBridgeWiFi.h>
#include <ModbusClientRTU.h>
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
#define NUM_VALUES 2

bool data_ready = false;
uint16_t values[NUM_VALUES];
uint16_t temp, humi;
uint32_t request_time;

// Define an onData handler function to receive the regular responses
// Arguments are received response message and the request's token
void handleData(ModbusMessage response, uint32_t token) 
{
  // First value is on pos 3, after server ID, function code and length byte
  uint16_t offs = 3;
  // The device has values all as IEEE754 float32 in two consecutive registers
  // Read the requested in a loop
  
  for (uint8_t i = 0; i < NUM_VALUES; ++i) {
    offs = response.get(offs, values[i]);
  }

  // HEXDUMP_N("Data", response.data(), response.size());

  // Signal "data is complete"
  request_time = token;
  data_ready = true;
}

// Define an onError handler function to receive error responses
// Arguments are the error code returned and a user-supplied token to identify the causing request
void handleError(Error error, uint32_t token) 
{
  // ModbusError wraps the error code and provides a readable error message for it
  ModbusError me(error);
  LOG_E("Error response: %02X - %s\n", (int)me, (const char *)me);
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
  MBclient->setTimeout(1000);
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

  modbusPollingInterval = config.getPollingInterval();
  modbusPollingSlaveId = config.getPollingSlaveId();
}

void loop() {
  static unsigned long next_request = millis();

  if (modbusPollingInterval >= 1000) {
    // Shall we do another request?
    if (millis() - next_request > modbusPollingInterval) {
      // Yes.
      data_ready = false;
      // Issue the request
      Error err = MBclient->addRequest((uint32_t)millis(), modbusPollingSlaveId, READ_HOLD_REGISTER, FIRST_REGISTER, NUM_VALUES);
      if (err!=SUCCESS) {
        ModbusError e(err);
        LOG_E("Error creating request: %02X - %s\n", (int)e, (const char *)e);
      }
      // Save current time to check for next cycle
      next_request = millis();
    } else {
      // No, but we may have another response
      if (data_ready) {
        // We do. Print out the data
        debugSerial.printf("Requested at %3.3fs:\n", request_time / 1000.0);
        for (uint8_t i = 0; i < NUM_VALUES; ++i) {
          debugSerial.printf("   %04X: %d\n", i + FIRST_REGISTER, values[i]);
        }
        debugSerial.printf("----------\n\n");
        data_ready = false;
      }
    }
  }
  // put your main code here, to run repeatedly:
}