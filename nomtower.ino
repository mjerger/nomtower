/* 
 *  nomtower
 */

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <uri/UriRegex.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <uptime.h>
#include <uptime_formatter.h>
#include "ESP8266TimerInterrupt.h"
#include "ESP8266_ISR_Timer.h"
#include <time.h>
#include <CircularBuffer.hpp>
#include "utils.h"

// create this file from secrets.h.template
#include "secrets.h"

// active config
JsonDocument config; 

// debug
#define SERIAL_ENABLED
//#define WAIT_FOR_CONNECT

// TIME /////////////////////////////////////

#define MY_NTP_SERVER "at.pool.ntp.org"
#define MY_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
time_t now;
tm tm;
const String weekdays[7]{ "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
const String weekdays_short[7]{ "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

String getTimeString(bool longDay = false)
{
  time(&now);
  localtime_r(&now, &tm);

  return (longDay ? weekdays[tm.tm_wday] : weekdays_short[tm.tm_wday]) + " " + 
         leadingZero(tm.tm_mday) + "." + 
         leadingZero(tm.tm_mon + 1) + "." + 
         String(tm.tm_year + 1900) + " " + 
         leadingZero(tm.tm_hour + 1) + ":" +  // No idea why it's off by one.. maybe DST is not working??
         leadingZero(tm.tm_min) + ":" + 
         leadingZero(tm.tm_sec);
}


// PINS /////////////////////////////////////

#define PIN_PUMP    D1
#define PIN_ACT_LED D2
#define PIN_TDS     A0
#define PIN_MOIST_1 D5
#define PIN_MOIST_2 D6
#define PIN_MOIST_3 D7

// signal activity
void activity()
{
  digitalWrite(PIN_ACT_LED, HIGH);
}


// PUMPS /////////////////////////////////////

#define PUMP_BUFF_LEN 256
struct pump_sample { bool val; long time; };
CircularBuffer<pump_sample, PUMP_BUFF_LEN> pump_samples;

bool pumpState;
void setPump(bool enable)
{
  if (enable == pumpState)
    return;

  pumpState = enable;
  digitalWrite(PIN_PUMP, pumpState);

  // log state
  time(&now);
  pump_sample sample = { enable, now };
  pump_samples.push(sample);

  activity();

  Serial.println("PUMP\t" + getTimeString() + "\t\t" + (enable ? "set on" : "set off"));
}


// select pump config by time
int selectPumpConfig()
{
  static int activePumpConfig = -1;
 
  time(&now);
  localtime_r(&now, &tm);

  // military time 0000 - 2359
  const int tnow = (tm.tm_hour+1) * 100 + tm.tm_min;

  // find the current time interval by looking at the start times
  JsonArray arr = config["pump"].as<JsonArray>();
  int current = -1;
  int lastOfDay = -1;
  for (int i = 0; i < arr.size(); i++) {
    const int t = arr[i]["start"];

    // element with the largest start time that is before NOW
    if ((current < 0 || t > arr[current]["start"]) && t <= tnow)
      current = i;

    // element with largest start time overall
    if (lastOfDay < 0 || t > arr[lastOfDay]["start"])
      lastOfDay = i;
  }

  // If we didn't find the current time range, it means we wrapped over to the next day, 
  // In that case we are running with the last profile of the previous day.
  const int selected = (current > -1 ? current : lastOfDay);
  
  // nothing changed
  if (activePumpConfig == selected)
    return activePumpConfig;
  
  String name = config["pump"][selected]["name"].as<String>();
  Serial.println("PUMP\t" + getTimeString() + "\t\t" + "switch profile '" + name + "'");

  activePumpConfig = selected;
  return activePumpConfig;  
}


void tickPump()
{
  static long last = 0;

  const int c = selectPumpConfig();
  const int interval = config["pump"][c]["interval"] | 10;
  const int duration = config["pump"][c]["duration"] | 5;

  time(&now);
  if (pumpState && (long)now >= last + interval) {
      setPump(false);
      last = now;
  } else if (!pumpState && (long)now >= last + duration) {
      setPump(true);
      last = now;
  }
}


// SENSORS /////////////////////////////////////

#define TDS_INTERVAL 1 /* seconds */
#define TDS_BUFF_LEN 256
struct tds_sample { int val; long time; };
CircularBuffer<tds_sample, TDS_BUFF_LEN> tds_samples;


int tickTDS()
{
  static tds_sample sample = {0, 0};

  // is it time to sample?
  time(&now);
  if (now < sample.time + (config["tds"]["interval"] | 1))
    return -1;

  // new sample
  sample = { analogRead(PIN_TDS), now };
  tds_samples.push(sample);

  activity();

  Serial.println("TDS\t" + getTimeString() + "\t\tread " + String(sample.val));

  return sample.val;
}


// CONFIG /////////////////////////////////////


void loadConfig()
{
  // load from file
  String filename = "/config.json";
  File file = SPIFFS.open(filename, "r");
  deserializeJson(config, file);
  file.close();

  // fill missing values with defaults
  const int count = config["pump"].as<JsonArray>().size();
  if (!count)
  {
    JsonArray pumpArr = config["pump"].to<JsonArray>();

    JsonObject day = pumpArr.add<JsonObject>();
    day["name"]       = "Day";
    day["start"]      = 1000; // 10am
    day["interval"]   = 600;  // every 10 mins 
    day["duration"]   = 30;   // turn on for 30 seconds

    JsonObject night = pumpArr.add<JsonObject>();
    night["name"]     = "Night";
    night["start"]    = 2000; // 8pm
    night["interval"] = 6000; // every hour 
    night["duration"] = 60;   // turn on for 60 seconds
  }

  // make sure there are valid values
  for (int i=0; i<count; i++)
  {
    config["pump"][i]["name"]     = config["pump"][i]["name"]     | String('#') + String(i);
    config["pump"][i]["start"]    = config["pump"][i]["start"]    | 1000;
    config["pump"][i]["interval"] = config["pump"][i]["interval"] | 600;
    config["pump"][i]["duration"] = config["pump"][i]["duration"] | 30;
  }
  
  // TDS
  config["tds"]["interval"] = config["tds"]["interval"] | 60;
}


void saveConfig() 
{
  String filename = "/config.json";
  File file = SPIFFS.open(filename, "w");
  String data;
  serializeJson(config, data);
  file.write(data.c_str(), data.length());
  file.close();
}


// SERVER /////////////////////////////////////

const String methods[8]{ "ANY", "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS" };
ESP8266WebServer server(80);
File uploadFile;

bool sendJson(JsonDocument json)
{
  String string;
  serializeJson(json, string);
  server.send(200, "application/json", string);
  return true;
}

bool sendText(String text)
{
  server.send(200, "text/plain", text);
  return true;
}

bool sendOK()
{
  server.send(200);
  return true;
}

bool sendRedirect(String location)
{
  server.sendHeader("Location", location);
  server.send(303);
  return true;
}

bool sendError()
{
  server.send(500, "text/plain", "500: Internal Server Error");
  return true;
}


bool handleSysinfo()
{
  JsonDocument json;

  // Uptime
  json["uptime"] = uptime_formatter::getUptime();

  // NTP time
  json["datetime"] = getTimeString(true);

  // Network
  json["ssid"] = String(ssid);
  json["host"] = String(mdnsHostname) + ".local";
  json["ip"] = WiFi.localIP();

  // Filesystem
  FSInfo fs_info;
  SPIFFS.info(fs_info);
  json["fs_free"] = String(double(fs_info.totalBytes - fs_info.usedBytes) / 1024.0, 1) + "k";
  json["fs_used"] = String((double)fs_info.usedBytes / 1024.0, 1) + "k";
  json["fs_total"] = String((double)fs_info.totalBytes / 1024.0, 1) + "k";

  return sendJson(json);
}


bool handlePump(String path)
{
  if (path.isEmpty()) 
  {
    return sendText(pumpState ? "1" : "0");
  }
  else if (path == "on") 
  {
    setPump(true);
    return sendOK();
  }
  else if (path == "off")
  {
    setPump(false);
    return sendOK();
  }
  else if (path == "log")
  {
    String csv;
    for (int i=0; i<pump_samples.size(); i++)
    {
      if (i>0) csv += "\n";
      csv += String(pump_samples[i].time) + "," + String(pump_samples[i].val);
    }

    server.send(200, "text/plain", csv);
    return true;  
  }

  return false;
}


bool handleTDS(String path)
{
  if (path.isEmpty()) {
    sendText(String(tds_samples.last().val));
  }
  else if (path == "log")
  {
    String csv;
    for (int i=0; i<tds_samples.size(); i++)
    {
      if (i>0) csv += "\n";
      csv += String(tds_samples[i].time) + "," + String(tds_samples[i].val);
    }

    server.send(200, "text/plain", csv);
    return true;  
  }

  return false;
}


// Response with file from FS
bool handleFile(String path) 
{
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";

  // Send requested (compressed) file
  if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {

    if (SPIFFS.exists(pathWithGz)) path += ".gz";

    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();

    return true;
  }

  return false;
}


bool handleSaveConfig()
{
  if (server.hasArg("config")) {

    String filename = "/config.json";
    File file = SPIFFS.open(filename, "w");
    String data = server.arg("config");
    JsonObject json;
    deserializeJson(json, data);
    // TODO apply config
    //saveConfig();

    return sendRedirect("/settings");
  }
  return sendError();
}


void setupServer(void)
{
  // routes
  server.on("/",                      HTTP_GET,  []() { activity(); handleFile("/home.html");      });
  server.on("/settings",              HTTP_GET,  []() { activity(); handleFile("/settings.html");  });
  server.on("/config",                HTTP_POST, []() { activity(); handleSaveConfig();  });
  server.on("/sysinfo",               HTTP_GET,   []() { activity(); handleSysinfo();  });
  server.on("/pump",                  HTTP_GET,   []() { activity(); handlePump("");  });
  server.on(UriRegex("/pump/(.*)"),   HTTP_GET,   []() { activity(); handlePump(server.pathArg(0));  });
  server.on("/tds",                   HTTP_GET,   []() { activity(); handleTDS("");  });
  server.on(UriRegex("/tds/(.*)"),    HTTP_GET,   []() { activity(); handleTDS(server.pathArg(0));  });
  
  // we use our own routing hacky hack
  server.onNotFound([]() {
    activity();
    Serial.print(methods[server.method()] + " -> " + server.uri());
    if (handleFile(server.uri())) {
      Serial.println("ok");
    } else {
      Serial.println("not found");
      server.send(404, "text/plain", "404: Not Found");
    }
  });

  // Start Server
  server.begin();
  Serial.println("HTTP server started");
}


void setup(void) 
{
  // LED 
  pinMode(PIN_ACT_LED, OUTPUT);
  digitalWrite(PIN_ACT_LED, LOW);

  // Pump, always on at start
  pinMode(PIN_PUMP, OUTPUT);
  setPump(true);

  //inputs
  pinMode(PIN_TDS, INPUT);
  pinMode(PIN_MOIST_1, INPUT);
  pinMode(PIN_MOIST_2, INPUT);
  pinMode(PIN_MOIST_3, INPUT);

  #ifdef SERIAL_ENABLED
    Serial.begin(115200);
  #endif
  
  Serial.println("\n\n");
  Serial.println("--------------");
  Serial.println(" nomtower 0.1");
  Serial.println("--------------");

  Serial.print("Initializing SPIFFS ... ");
  if (SPIFFS.begin()) {
    Serial.println("ok");
  } else {
    Serial.println("failed");
    errorLoop(PIN_ACT_LED);
  }

  loadConfig();
  
  // init config on first boot
  if (!SPIFFS.exists("/config.json"))
    saveConfig();

  // setup routes
  setupServer();

  // connect to WIFI
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to SSID ");
  Serial.print(ssid);
  #ifdef WAIT_FOR_CONNECT
    Serial.print(" ");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println(" ok");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  #else
    Serial.println();  
  #endif

  // start MSDN responder
  if (MDNS.begin(mdnsHostname)) {
    Serial.println("MDNS responder started: " + String(mdnsHostname) + ".local");
  }

}


void loop(void) 
{
  // handle network
  server.handleClient();
  MDNS.update();

  // handle our stuff
  tickTDS();
  tickPump();

  delay(50);

  // reset activity led
  digitalWrite(PIN_ACT_LED, LOW);
}
