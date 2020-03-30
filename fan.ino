/*
 * module is a esp-12
 * flash size set to 4M (1M SPIFFS)
 */


#include <ESP8266WiFi.h>
#include <WebSocketsServer.h>
#include <Hash.h>
#include <TimeLib.h> 
//#include <Timezone.h>
#include <ArduinoJson.h>
#include <EEPROM.h>

// --------------------------------------------

#include <ESP8266WebServer.h>
#include <FS.h>

// --------------------------------------------

// wifi manager includes
#include <DNSServer.h>
#include <WiFiManager.h>

// --------------------------------------------

// aync library includes
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>

// --------------------------------------------

// amazon alexa support
#include <Espalexa.h>

// --------------------------------------------

//US Eastern Time Zone (New York, Detroit)
//TimeChangeRule myDST = {"EDT", Second, Sun, Mar, 2, -240};    //Daylight time = UTC - 4 hours
//TimeChangeRule mySTD = {"EST", First, Sun, Nov, 2, -300};     //Standard time = UTC - 5 hours
//Timezone myTZ(myDST, mySTD);

const char *weekdayNames[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// --------------------------------------------

#define DEFAULT_OUTPUT HIGH
// note: direction is not implemented
//                 off, low, medium, high, direction, top, bottom
byte outputs[] = { 15,  14,  13,     12,   0,         16,  5 }; 
const char *actionNames[] = {"Off", "Low", "Medium", "High", "Direction", "Top", "Bottom"};

bool isSetup = false;
unsigned long lastMinutes;

#define OFF    0
#define RUN    1
const char *modeNames[] = { "Off", "Running" };


#define NONE         -1
#define TIME_OFF     255
#define NUM_PROGRAMS 4
#define NUM_TIMES    4


typedef struct {
  byte isEnabled;
  byte dayMask;
  byte startTime[NUM_TIMES];
  byte action[NUM_TIMES];
} programType;

programType program[NUM_PROGRAMS];

Espalexa espalexa;
ESP8266WebServer server(80);
File fsUploadFile;
bool isUploading;

WebSocketsServer webSocket = WebSocketsServer(81);
int webClient = -1;
int programClient = -1;
int setupClient = -1;

bool isTimeSet = false;

bool isPromModified;
bool isMemoryReset = false;
//bool isMemoryReset = true;

typedef struct {
  byte mode;
} configType;

configType config;

void loadConfig(void);
void loadProgramConfig(void);
bool setupWifi(void);
void setupTime(void);
void setupOutputs(void);
void setupWebServer(void);
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght);
void drawMainScreen(void);
void set(char *name, const char *value);
void saveProgramConfig(void);
unsigned long sendNTPpacket(IPAddress& address);
void printTime(bool isCheckProgram, bool isTest);
void printModeState(void);
void sendWeb(const char *command, const char *value);
void checkTimeMinutes(void);
void saveConfig(void);
void startProgram(int index, int startIndex);
void checkProgram(int day, int h, int m);
void update(int addr, byte data);
void printRunning(void);
void modeChange(void);
void doAction(int action);


void setup(void) {
  // start serial port
  Serial.begin(115200);
  Serial.print(F("\n\n"));

  Serial.println(F("esp8266 fan"));
  Serial.println(F("compiled:"));
  Serial.print( __DATE__);
  Serial.print(F(","));
  Serial.println( __TIME__);

  setupOutputs();
  
  if (!setupWifi())
    return;
    
  // must specify amount of eeprom to use (max is 4k?)
  EEPROM.begin(512);
  
  loadConfig();
  loadProgramConfig();
  isMemoryReset = false;

  setupTime();
  setupWebServer();  

  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
    
  lastMinutes = 0;

  isUploading = false;

  isSetup = true;
  modeChange();
}

void setupOutputs(void) {
  int numOutputs = sizeof(outputs)/sizeof(outputs[0]);
  for (int i=0; i < numOutputs; ++i) {
    pinMode(outputs[i], OUTPUT);
    digitalWrite(outputs[i], DEFAULT_OUTPUT);
  }
}

void initProgram(void) {
/*
3:30am, tue and sat

each day is a bit: sun is lsb, sat is msb
mon thru fri: B00111110
sat and sun:  B01000001
1
time starts at 12am = 0, and goes in 15 minute increments
4:00pm: (4+12)*4
*/
  for (int i=0; i < NUM_PROGRAMS; ++i) {
    program[i].isEnabled = false;
    program[i].dayMask = B00000000;
    for (int j=0; j < NUM_TIMES; ++j) {
      program[i].startTime[j] = TIME_OFF;
      program[i].action[j] = OFF;
    }
  }
  
//  program[0].isEnabled = true;
//  program[0].dayMask = B00111110;      // mon thru fri
//  program[0].startTime[0] = (4+12)*4;  // 4pm
//
//  program[1].isEnabled = true;
//  program[1].dayMask = B01000001;      // sat and sun
//  program[1].startTime[0] = (12)*4;    // noon

  saveProgramConfig(); 
}

#define AP_NAME "Fan"

void configModeCallback(WiFiManager *myWiFiManager) {
  // this callback gets called when the enter AP mode, and the users
  // need to connect to us in order to configure the wifi
  Serial.print(F("\n\nJoin: "));
  Serial.println(AP_NAME);
  Serial.print(F("Goto: "));
  Serial.println(WiFi.softAPIP());
  Serial.println();
}

bool setupWifi(void) {
  WiFi.hostname("fan");
  
  WiFiManager wifiManager;
//  wifiManager.setDebugOutput(false);
  
  //reset settings - for testing
  //wifiManager.resetSettings();

  String ssid = WiFi.SSID();
  if (ssid.length() > 0) {
    Serial.print(F("Connecting to "));
    Serial.println(ssid);
  }
  
  //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wifiManager.setAPCallback(configModeCallback);

  if(!wifiManager.autoConnect(AP_NAME)) {
    Serial.println(F("failed to connect and hit timeout"));
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(1000);
  } 

  return true;
}


void setupTime(void) {
  Serial.println(F("Getting time"));

  AsyncUDP* udp = new AsyncUDP();

  // time.nist.gov NTP server
  // NTP requests are to port 123
  if (udp->connect(IPAddress(129,6,15,28), 123)) {
//    Serial.println("UDP connected");
    
    udp->onPacket([](void *arg, AsyncUDPPacket packet) {
//      Serial.println(F("received NTP packet"));
      byte *buf = packet.data();
      
      //the timestamp starts at byte 40 of the received packet and is four bytes,
      // or two words, long. First, esxtract the two words:
    
      // convert four bytes starting at location 40 to a long integer
      unsigned long secsSince1900 =  (unsigned long)buf[40] << 24;
      secsSince1900 |= (unsigned long)buf[41] << 16;
      secsSince1900 |= (unsigned long)buf[42] << 8;
      secsSince1900 |= (unsigned long)buf[43];
      time_t utc = secsSince1900 - 2208988800UL;
    
    // cpd..hack until timezone is fixed
//      utc -= 60 * 60 * 4;  // spring foward
      utc -= 60 * 60 * 5;  // fall back
      setTime(utc);
    
//      TimeChangeRule *tcr;
//      time_t local = myTZ.toLocal(utc, &tcr);
//      Serial.printf("\ntime zone %s\n", tcr->abbrev);
//    
//      setTime(local);
    
      // just print out the time
      printTime(false, true);
    
      isTimeSet = true;

      free(arg);
    }, udp);
    
//    Serial.println(F("sending NTP packet"));

    const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
    byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold outgoing packet

    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;
    
    // all NTP fields have been given values, now
    // send a packet requesting a timestamp:
    udp->write(packetBuffer, NTP_PACKET_SIZE);
  }
  else {
    free(udp);
    Serial.println(F("\nWiFi:time failed...will retry in a minute"));
  }
}


void drawMainScreen(void) {
  printTime(true, false);
  printModeState();
}


void printModeState(void) {
  if (webClient != -1) {
    const char *msg;
    if (config.mode == OFF)
      msg = "Off";
    else
      msg = "Running";

    char buf[3];
    sprintf(buf, "%d", config.mode);    
    sendWeb("mode", buf);
    sendWeb("modeState", msg);
  }
}

void loop(void)
{
  if (isUploading) {
    // you can omit this line from your code since it will be called in espalexa.loop()
    // server.handleClient();
    espalexa.loop();
    return;
  }
  
  if (!isSetup)
    return;
   
  checkTimeMinutes();

  webSocket.loop();
  
  // you can omit this line from your code since it will be called in espalexa.loop()
  // server.handleClient();
  espalexa.loop();
}

void checkTimeMinutes() {
  int minutes = minute();
  if (minutes == lastMinutes)
    return;

  // resync time at 3am every morning
  // this also catches daylight savings time changes which happen at 2am
  if (minutes == 0 && hour() == 3)
    isTimeSet = false;

  if (!isTimeSet)
    setupTime();
  
  lastMinutes = minutes;
  printTime(true, false);
}

void modeChange(void) {
  saveConfig();

  drawMainScreen();
  printModeState();
}

void sendWeb(const char *command, const char *value) {
  char json[128];
  sprintf(json, "{\"command\":\"%s\",\"value\":\"%s\"}", command, value);
  webSocket.sendTXT(webClient, json, strlen(json));
}

void printTime(bool isCheckProgram, bool isTest) {
  int dayOfWeek = weekday()-1;
  int hours = hour();
  int minutes = minute();

  if (webClient != -1 || isTest) {
    const char *ampm = "a";
    int h = hours;
    if (h == 0)
      h = 12;
    else if (h == 12)
      ampm = "p";
    else if (h > 12) {
      h -= 12;
      ampm = "p";
    }
    char buf[7];
    sprintf(buf, "%2d:%02d%s", h, minutes, ampm); 

    char msg[6+1+4];
    sprintf(msg, "%s %s", buf, weekdayNames[dayOfWeek]); 
    if (webClient != -1)
      sendWeb("time", msg);
    if (isTest)
      Serial.printf("time is %s\n", msg);
  }

  if (isCheckProgram) {
    // see if the program needs to be started
    checkProgram(dayOfWeek, hours, minutes);
  }
}

void startProgram(int index, int startIndex) {
  printModeState();

  int action = program[index].action[startIndex];
  Serial.printf("starting program %d-%d: %d\n", (index+1), (startIndex+1), action);
  doAction(action);
}

void doAction(int action) {
  // press the button for 500 msec
  int pin = outputs[action];
  digitalWrite(pin, !DEFAULT_OUTPUT);
  delay(500);
  digitalWrite(pin, DEFAULT_OUTPUT);

  Serial.printf("action %s on gpio: %d\n", actionNames[action], pin);
  if (webClient != -1)
    sendWeb("status", actionNames[action]);
}

void checkProgram(int day, int h, int m) {
  if (config.mode == OFF)
    return;
    
  // check each program
  int ctime = h*60+m;
  for (int i=0; i < NUM_PROGRAMS; ++i) {
    if ( !program[i].isEnabled )
      continue;
    
    // check day
    if (((1 << day) & program[i].dayMask) == 0)
      continue;
    
    // check each start time
    for (int j=0; j < NUM_TIMES; ++j) {
      if (program[i].startTime[j] == TIME_OFF)
        continue;
        
      int ptime = program[i].startTime[j]*15;
      if (ptime == ctime) {
        startProgram(i, j); 
        return;
      }
    }
  }
  
  // no programs were matches
}

#define MAGIC_NUM   0xAD

#define MAGIC_NUM_ADDRESS      0
#define CONFIG_ADDRESS         1
#define PROGRAM_ADDRESS        CONFIG_ADDRESS + sizeof(config)

void set(char *name, const char *value) {
  for (int i=strlen(value); i >= 0; --i)
    *(name++) = *(value++);
}

void loadConfig(void) {
  int magicNum = EEPROM.read(MAGIC_NUM_ADDRESS);
  if (magicNum != MAGIC_NUM) {
    Serial.println(F("invalid eeprom data"));
    isMemoryReset = true;
  }
  
  if (isMemoryReset) {
    // nothing saved in eeprom, use defaults
    Serial.println(F("using default config"));
    config.mode = OFF;

    saveConfig();
  }
  else {
    int addr = CONFIG_ADDRESS;
    byte *ptr = (byte *)&config;
    for (int i=0; i < sizeof(config); ++i, ++ptr)
      *ptr = EEPROM.read(addr++);
  }

  Serial.printf("mode %d\n", config.mode);
}

void loadProgramConfig(void) {
  if (isMemoryReset) {
    // nothing saved in eeprom, use defaults
    Serial.printf("using default programs\n");
    initProgram();  
  }
  else {
    Serial.printf("loading programs from eeprom\n");
    int addr = PROGRAM_ADDRESS;
    byte *ptr = (byte *)&program;
    for (int i = 0; i < sizeof(program); ++i, ++ptr, ++addr)
      *ptr = EEPROM.read(addr);
  }
}

void saveConfig(void) {
  isPromModified = false;
  update(MAGIC_NUM_ADDRESS, MAGIC_NUM);

  byte *ptr = (byte *)&config;
  int addr = CONFIG_ADDRESS;
  for (int j=0; j < sizeof(config); ++j, ++ptr)
    update(addr++, *ptr);
  
  if (isPromModified)
    EEPROM.commit();
}

void update(int addr, byte data) {
  if (EEPROM.read(addr) != data) {
    EEPROM.write(addr, data);
    isPromModified = true;
  }
}

void saveProgramConfig(void) {
  isPromModified = false;
  Serial.printf("saving programs to eeprom\n");
  int addr = PROGRAM_ADDRESS;
  byte *ptr = (byte *)&program;
  for (int i = 0; i < sizeof(program); ++i, ++ptr, ++addr)
    update(addr, *ptr);

  if (isPromModified)
    EEPROM.commit();
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t lenght) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected!\n", num);
      if (num == webClient)
        webClient = -1;
      else if (num == programClient)
        programClient = -1;
      else if (num == setupClient)
        setupClient = -1;
      break;
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
      }
      
      if (strcmp((char *)payload,"/") == 0) {
        webClient = num;
      
        // send the current state
        printModeState();
        printTime(false, false);
      }
      else if (strcmp((char *)payload,"/program") == 0) {
        programClient = num;
        
        // send programs
        char json[265*2];
        strcpy(json, "{\"command\":\"program\",\"value\":[");
        for (int i=0; i < NUM_PROGRAMS; ++i) {
          sprintf(json+strlen(json), "%s[%d,%d,[", (i==0)?"":",", program[i].isEnabled, program[i].dayMask);
          for (int j=0; j < NUM_TIMES; ++j)
            sprintf(json+strlen(json), "%d%s", program[i].startTime[j], (j==NUM_TIMES-1)?"],[":",");
          for (int j=0; j < NUM_TIMES; ++j)
            sprintf(json+strlen(json), "%d%s", program[i].action[j], (j==NUM_TIMES-1)?"]]":",");
        }
        strcpy(json+strlen(json), "]}");
        //Serial.printf("len %d\n", strlen(json));
        webSocket.sendTXT(programClient, json, strlen(json));
      }
      else if (strcmp((char *)payload,"/setup") == 0) {
        setupClient = num;

        char json[256];
        strcpy(json, "{");
        sprintf(json+strlen(json), "\"date\":\"%s\"", __DATE__);
        sprintf(json+strlen(json), ",\"time\":\"%s\"", __TIME__);
        strcpy(json+strlen(json), "}");
//        Serial.printf("len %d\n", strlen(json));
        webSocket.sendTXT(setupClient, json, strlen(json));
      }
      else {
        Serial.printf("unknown call %s\n", payload);
      }
      break;
    case WStype_TEXT:
      Serial.printf("[%u] get Text: %s\n", num, payload);
      
      if (num == webClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"mode",4) == 0) {
          target = "value";
          ptr = strstr(ptr, target) + strlen(target)+3;
          config.mode = strtol(ptr, &ptr, 10);
          modeChange();
        }        
        else if (strncmp(ptr,"button",6) == 0) {
          target = "value";
          ptr = strstr(ptr, target) + strlen(target)+2;
          int action = strtol(ptr, &ptr, 10);
          doAction(action);
        }        
      }
      else if (num == programClient) {
        Serial.printf("save programs\n");
        char *ptr = strchr((char *)payload, '[')+2;
        for (int i=0; i < NUM_PROGRAMS; ++i) {
          program[i].isEnabled = strtol(ptr, &ptr, 10);
          ptr += 1;

          program[i].dayMask = strtol(ptr, &ptr, 10);
          ptr += 2;

          for (int j=0; j < NUM_TIMES; ++j, ++ptr) {
            program[i].startTime[j] = strtol(ptr, &ptr, 10);
//            Serial.printf("startTime %d %d %d\n", i, j, program[i].startTime[j]);
          }
          ptr += 2;
          for (int j=0; j < NUM_TIMES; ++j, ++ptr) {
            program[i].action[j] = strtol(ptr, &ptr, 10);
//            Serial.printf("action %d %d %d\n", i, j, program[i].action[j]);
          }
          ptr += 3;
        }      
        saveProgramConfig();
        drawMainScreen();
      }
      else if (num == setupClient) {
        const char *target = "command";
        char *ptr = strstr((char *)payload, target) + strlen(target)+3;
        if (strncmp(ptr,"reboot",6) == 0) {
          ESP.restart();
        }
        else if (strncmp(ptr,"save",4) == 0) {
          Serial.printf("save setup\n");
          
          saveConfig();
        }
      }
      break;
  }
}

void setUpOta(void) {
  server.on("/update", HTTP_POST, [](){
    server.sendHeader("Connection", "close");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
    ESP.restart();
  },[](){
    HTTPUpload& upload = server.upload();
    if(upload.status == UPLOAD_FILE_START){
      Serial.setDebugOutput(true);
      Serial.printf("Update filename: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if(!Update.begin(maxSketchSpace)){//start with max available size
        Update.printError(Serial);
      }
    } else if(upload.status == UPLOAD_FILE_WRITE){
      Serial.printf("uploaded: %d\n", upload.totalSize);
      if(Update.write(upload.buf, upload.currentSize) == upload.currentSize){
        // update the total percent complete on the web page
        // cpd
      }
      else {
        Update.printError(Serial);
      }
    } else if(upload.status == UPLOAD_FILE_END){
      if(Update.end(true)){ //true to set the size to the current progress
        Serial.printf("Update Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
      Serial.setDebugOutput(false);
    }
    yield();
  });
}

//format bytes
String formatBytes(size_t bytes){
  if (bytes < 1024){
    return String(bytes)+"B";
  } else if(bytes < (1024 * 1024)){
    return String(bytes/1024.0)+"KB";
  } else if(bytes < (1024 * 1024 * 1024)){
    return String(bytes/1024.0/1024.0)+"MB";
  } else {
    return String(bytes/1024.0/1024.0/1024.0)+"GB";
  }
}

String getContentType(String filename){
  if(server.hasArg("download")) return "application/octet-stream";
  else if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  else if(filename.endsWith(".xml")) return "text/xml";
  else if(filename.endsWith(".pdf")) return "application/x-pdf";
  else if(filename.endsWith(".zip")) return "application/x-zip";
  else if(filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path){
  Serial.println("handleFileRead: " + path);
  if(path.endsWith("/")) path += "index.htm";
  String contentType = getContentType(path);
  String pathWithGz = path + ".gz";
  if(SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)){
    if(SPIFFS.exists(pathWithGz))
      path += ".gz";
    File file = SPIFFS.open(path, "r");
    size_t sent = server.streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

void handleFileUpload_edit(){
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    String filename = upload.filename;
    if(!filename.startsWith("/")) filename = "/"+filename;
    Serial.print("handleFileUpload Name: "); Serial.println(filename);
    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();
  } else if(upload.status == UPLOAD_FILE_WRITE){
    //Serial.print("handleFileUpload Data: "); Serial.println(upload.currentSize);
    if(fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(fsUploadFile)
      fsUploadFile.close();
    Serial.print("handleFileUpload Size: "); Serial.println(upload.totalSize);
  }
}

void handleFileDelete(){
  if(server.args() == 0) return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileDelete: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(!SPIFFS.exists(path))
    return server.send(404, "text/plain", "FileNotFound");
  SPIFFS.remove(path);
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileCreate(){
  if(server.args() == 0)
    return server.send(500, "text/plain", "BAD ARGS");
  String path = server.arg(0);
  Serial.println("handleFileCreate: " + path);
  if(path == "/")
    return server.send(500, "text/plain", "BAD PATH");
  if(SPIFFS.exists(path))
    return server.send(500, "text/plain", "FILE EXISTS");
  File file = SPIFFS.open(path, "w");
  if(file)
    file.close();
  else
    return server.send(500, "text/plain", "CREATE FAILED");
  server.send(200, "text/plain", "");
  path = String();
}

void handleFileList() {
  if(!server.hasArg("dir")) {server.send(500, "text/plain", "BAD ARGS"); return;}
  
  String path = server.arg("dir");
  Serial.println("handleFileList: " + path);
  Dir dir = SPIFFS.openDir(path);
  path = String();

  String output = "[";
  while(dir.next()){
    File entry = dir.openFile("r");
    if (output != "[") output += ',';
    bool isDir = false;
    output += "{\"type\":\"";
    output += (isDir)?"dir":"file";
    output += "\",\"name\":\"";
    output += String(entry.name()).substring(1);
    output += "\"}";
    entry.close();
  }
  
  output += "]";
  server.send(200, "text/json", output);
}

void countRootFiles(void) {
  int num = 0;
  size_t totalSize = 0;
  Dir dir = SPIFFS.openDir("/");
  while (dir.next()) {
    ++num;
    String fileName = dir.fileName();
    size_t fileSize = dir.fileSize();
    totalSize += fileSize;
    Serial.printf("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
  }
  Serial.printf("FS File: serving %d files, size: %s from /\n", num, formatBytes(totalSize).c_str());
}

void fanChanged(uint8_t brightness) {
  Serial.print("Alexa changed to ");
  Serial.println(brightness);
    
//  if (brightness == 255) {
//    config.mode = ON;
//  }
//  else if (brightness == 0) {
//    config.mode = OFF;
//  }
//  else {
//    // dim...not supported
//    return;
//  }
//  modeChange();
}

void setupWebServer(void) {
  SPIFFS.begin();

  countRootFiles();

  //list directory
  server.on("/list", HTTP_GET, handleFileList);
  
  //load editor
  server.on("/edit", HTTP_GET, [](){
    if(!handleFileRead("/edit.htm")) server.send(404, "text/plain", "FileNotFound");
  });
  
  //create file
  server.on("/edit", HTTP_PUT, handleFileCreate);
  
  //delete file
  server.on("/edit", HTTP_DELETE, handleFileDelete);
  
  //first callback is called after the request has ended with all parsed arguments
  //second callback handles file uploads at that location
  server.on("/edit", HTTP_POST, [](){ server.send(200, "text/plain", ""); }, handleFileUpload_edit);

  setUpOta();

  //called when the url is not defined here
  //use it to load content from SPIFFS
  server.onNotFound([](){
    if(!handleFileRead(server.uri())) {
      // if you don't know the URI, ask espalexa whether it is an Alexa control request
      if (!espalexa.handleAlexaApiCall(server.uri(),server.arg(0))) {
        server.send(404, "text/plain", "FileNotFound");
      }
    }
  });

  // alexa setup
  // simplest definition, default state off
  espalexa.addDevice("Fan", fanChanged);

  // give espalexa a pointer to your server object so it can use your server
  // instead of creating its own
  espalexa.begin(&server);
  //server.begin(); //omit this since it will be done by espalexa.begin(&server)

  Serial.println("HTTP and alexa server started");
}
