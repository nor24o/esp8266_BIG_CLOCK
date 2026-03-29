#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <math.h>
#include <stdarg.h>
#include <NTPClient.h>
#include <ESP8266WebServer.h>
#include <Preferences.h>
#include <WebSocketsServer.h>

// --- PIN DEFINITIONS AND CONSTANTS ---
const int DATA_PIN = 12;
const int LATCH_PIN = 13;
const int CLOCK_PIN = 14;
const int DIGITS_VCC = 16;

const byte digitPatterns_anode[10] = {
    0b00111111, 0b00000110, 0b01011011, 0b01001111, 0b01100110,
    0b01101101, 0b01111101, 0b00000111, 0b01111111, 0b01101111};
const byte MINUS_PATTERN = 0b01000000;
const byte BLANK_PATTERN = 0b00000000;

// --- PREFERENCES & CONFIG VARIABLES ---
Preferences preferences;
String wifi_ssid1, wifi_pass1;
String wifi_ssid2, wifi_pass2;
long utcOffsetInSeconds;
bool dstEnabled;
bool use12hFormat;

bool autoBrightnessEnabled;
int manualBrightness;
int h_day, h_dusk, h_night;
int b_day, b_dusk, b_night;

// --- WIFI, WEB SERVER & WEBSOCKETS ---
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
bool isAPMode = false;
String currentDisplayStr = "";
String lastDisplayStr = "";

// --- ANIMATION DATA ---
const int NUM_ANIM_STEPS = 28;
const byte animationFrames[NUM_ANIM_STEPS][4] = {
    {0b00000001, 0, 0, 0}, {0b00100000, 0, 0, 0}, {0b00010000, 0, 0, 0},
    {0b00001000, 0, 0, 0}, {0, 0b00010000, 0, 0}, {0, 0b00100000, 0, 0},
    {0, 0b00000001, 0, 0}, {0, 0b00000010, 0, 0}, {0, 0b00000100, 0, 0},
    {0, 0b10000000, 0, 0}, {0, 0, 0b00010000, 0}, {0, 0, 0b00100000, 0},
    {0, 0, 0b00000001, 0}, {0, 0, 0b00000010, 0}, {0, 0, 0b00000100, 0},
    {0, 0, 0b10000000, 0}, {0, 0, 0, 0b00010000}, {0, 0, 0, 0b00100000},
    {0, 0, 0, 0b00000001}, {0, 0, 0, 0b00000010}, {0, 0, 0, 0b00000100},
    {0, 0, 0, 0b00001000}, {0, 0, 0b10000000, 0}, {0, 0, 0b00001000, 0},
    {0, 0b10000000, 0, 0}, {0, 0b00001000, 0, 0}, {0b00000100, 0, 0, 0},
    {0b00000010, 0, 0, 0}};
const byte CONF[4] = {0b01011000, 0b01011100, 0b01010100, 0b01110001};

// --- STATE MANAGEMENT ---
enum DisplayMode { IDLE_TIME, OVERRIDE, ANIMATION };
DisplayMode currentMode = IDLE_TIME;
unsigned long overrideStartTime = 0;
const unsigned long OVERRIDE_DURATION = 20000; 

int hh = 0, mm = 0, ss = 0;
int lastHourChecked = -1;
uint32_t targetTime = 0;
bool timeSuccessfullySet = false;
unsigned long lastNtpAttempt = 0;

// --- NETWORKING OBJECTS ---
WiFiServer telnetServer(23);
WiFiClient telnetClient;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
const unsigned int UDP_PORT = 4210;
WiFiUDP udpListener;
char packetBuffer[255];

// --- LOGGING ---
class SerialMirror : public Print {
public:
  size_t write(uint8_t c) override {
    Serial.write(c);
    if (telnetClient && telnetClient.connected()) telnetClient.write(c);
    return 1;
  }
  void printf(const char *format, ...) {
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    print(buf);
  }
};
SerialMirror TelnetLogger;
#define LOG_PRINT(...) TelnetLogger.print(__VA_ARGS__)
#define LOG_PRINTLN(...) TelnetLogger.println(__VA_ARGS__)
#define LOG_PRINTF(...) TelnetLogger.printf(__VA_ARGS__)

// --- FUNCTION PROTOTYPES ---
void loadSettings();
void setupWebServerRoutes();
void applyNtpOffset();
void setAutoBrightness();
void displayAnimationStep(int step);
byte getPattern(int digit, bool dpOn);
void updateDisplay(byte p4, byte p3, byte p2, byte p1);
void displayFloat(float num);
void displayInteger(int num);
void displayTemperature(float temp);
void displayTime(int hours, int minutes, bool colonOn);
void handleUdpCommands();
bool connectToWiFi(String ssid, String pass, int timeoutSec);

// =======================================================
// ==================== SETUP ============================
// =======================================================
void setup() {
  Serial.begin(115200);
  LOG_PRINTLN("\nBooting");

  pinMode(LATCH_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(DATA_PIN, OUTPUT);
  pinMode(DIGITS_VCC, OUTPUT);
  
  loadSettings();

  int bootFails = preferences.getInt("fails", 0);
  bootFails++;
  preferences.putInt("fails", bootFails);
  
  analogWrite(DIGITS_VCC, autoBrightnessEnabled ? 150 : manualBrightness);
  updateDisplay(CONF[0], CONF[1], CONF[2], CONF[3]);
  currentDisplayStr = "CONF";
  delay(1000);

  if (bootFails >= 5) {
    LOG_PRINTLN("\n5 Restarts Detected! Starting AP Mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Clock_Config");
    isAPMode = true;
    preferences.putInt("fails", 0); 
    LOG_PRINTLN(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_STA);
    
    // Attempt Primary WiFi
    bool connected = connectToWiFi(wifi_ssid1, wifi_pass1, 10);
    
    // Attempt Secondary WiFi if Primary fails
    if (!connected && wifi_ssid2.length() > 0) {
      LOG_PRINTLN("Primary failed. Trying Secondary WiFi...");
      connected = connectToWiFi(wifi_ssid2, wifi_pass2, 10);
    }

    if (!connected) {
      LOG_PRINTLN("\nAll WiFi Connections Failed! Rebooting...");
      delay(1000);
      ESP.restart(); 
    }

    LOG_PRINTLN("\nWiFi Connected!");
    preferences.putInt("fails", 0); 
    
    ArduinoOTA.setHostname("esp8266_CLOCK2");
    ArduinoOTA.setPassword("admin");
    ArduinoOTA.begin();
    
    telnetServer.begin();
    telnetServer.setNoDelay(true);
    udpListener.begin(UDP_PORT);
    
    timeClient.begin();
    applyNtpOffset();
    if (timeClient.forceUpdate()) { 
      timeSuccessfullySet = true;
      hh = timeClient.getHours();
      mm = timeClient.getMinutes();
      ss = timeClient.getSeconds();
    }
    lastNtpAttempt = millis(); 
  }

  setupWebServerRoutes();
  server.begin();
  webSocket.begin();
  LOG_PRINTLN("HTTP & WebSocket servers started");

  if (!isAPMode) {
    if (autoBrightnessEnabled) setAutoBrightness();
    lastHourChecked = hh;
    targetTime = millis();
  }
}

// =======================================================
// ==================== LOOP =============================
// =======================================================
void loop() {
  server.handleClient();
  webSocket.loop();

  // Broadcast display changes over WebSocket
  if (currentDisplayStr != lastDisplayStr) {
    webSocket.broadcastTXT(currentDisplayStr);
    lastDisplayStr = currentDisplayStr;
  }

  if (!isAPMode) {
    unsigned long currentMillis = millis();
    unsigned long ntpInterval = timeSuccessfullySet ? 3600000UL : 30000UL; 

    if (WiFi.status() == WL_CONNECTED && (currentMillis - lastNtpAttempt > ntpInterval)) {
      if (timeClient.update()) {
        hh = timeClient.getHours();
        mm = timeClient.getMinutes();
        ss = timeClient.getSeconds();
        timeSuccessfullySet = true; 
      }
      lastNtpAttempt = currentMillis; 
    }

    ArduinoOTA.handle();
    handleUdpCommands();

    if (telnetServer.hasClient()) {
      if (!telnetClient || !telnetClient.connected()) {
        if (telnetClient) telnetClient.stop();
        telnetClient = telnetServer.accept();
      } else {
        telnetServer.accept().stop();
      }
    }

    if (telnetClient && telnetClient.connected() && telnetClient.available()) {
      String input = telnetClient.readStringUntil('\n');
      input.trim();
      if (input.length() > 0) {
        char command = input.charAt(0);
        String argument = input.substring(1);
        argument.trim();

        if (command == 'f' || command == 'i' || command == 't' || command == 'm') {
          currentMode = (command == 'm') ? ANIMATION : OVERRIDE;
          overrideStartTime = millis();
          if (command == 'f') displayFloat(argument.toFloat());
          if (command == 'i') displayInteger(argument.toInt());
          if (command == 't') displayTemperature(argument.toFloat());
        }
      }
    }

    if (targetTime < millis()) {
      targetTime += 1000;
      ss++;
      if (ss > 59) { ss = 0; mm++; if (mm > 59) { mm = 0; hh++; if (hh > 23) hh = 0; } }
    }

    if (autoBrightnessEnabled && hh != lastHourChecked) {
      setAutoBrightness();
      lastHourChecked = hh;
    }

    if ((currentMode == OVERRIDE || currentMode == ANIMATION) && (millis() - overrideStartTime > OVERRIDE_DURATION)) {
      currentMode = IDLE_TIME;
    }
    
    if (currentMode == IDLE_TIME) {
      displayTime(hh, mm, (ss % 2 == 0));
    } else if (currentMode == ANIMATION) {
      displayAnimationStep(((millis() - overrideStartTime) / 50) % NUM_ANIM_STEPS);
      currentDisplayStr = "ANIM";
    }
  }
}

// =======================================================
// ==================== SETTINGS & WEB ===================
// =======================================================

bool connectToWiFi(String ssid, String pass, int timeoutSec) {
  LOG_PRINT("Connecting to "); LOG_PRINTLN(ssid);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long timeout = millis() + (timeoutSec * 1000);
  int animStep = 0;
  
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() > timeout) return false;
    displayAnimationStep(animStep);
    currentDisplayStr = "WIFI";
    animStep = (animStep + 1) % NUM_ANIM_STEPS;
    delay(50);
  }
  return true;
}

void loadSettings() {
  preferences.begin("clock_cfg", false);
  wifi_ssid1 = preferences.getString("ssid1", "Link2");
  wifi_pass1 = preferences.getString("pass1", "Horvat2017");
  wifi_ssid2 = preferences.getString("ssid2", "");
  wifi_pass2 = preferences.getString("pass2", "");
  
  utcOffsetInSeconds = preferences.getLong("utc", 2 * 3600);
  dstEnabled = preferences.getBool("dst", false);
  use12hFormat = preferences.getBool("u12h", false);
  
  autoBrightnessEnabled = preferences.getBool("auto_br", true);
  manualBrightness = preferences.getInt("man_br", 150);
  
  h_day = preferences.getInt("h_day", 7);
  h_dusk = preferences.getInt("h_dusk", 18);
  h_night = preferences.getInt("h_night", 22);
  
  b_day = preferences.getInt("b_day", 255);
  b_dusk = preferences.getInt("b_dusk", 50);
  b_night = preferences.getInt("b_night", 3);
}

void applyNtpOffset() {
  long totalOffset = utcOffsetInSeconds + (dstEnabled ? 3600 : 0);
  timeClient.setTimeOffset(totalOffset);
}

void setupWebServerRoutes() {
  server.on("/", HTTP_GET, []() {
    String html = R"rawliteral(
    <!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Clock Dashboard</title>
    <style>
      body { font-family: Arial; margin: 0; padding: 20px; background: #e9ecef; }
      .container { background: white; padding: 20px; border-radius: 8px; max-width: 500px; margin: auto; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
      .live-view { background: #111; color: #0f0; font-family: 'Courier New', monospace; font-size: 3rem; text-align: center; padding: 20px; border-radius: 8px; margin-bottom: 20px; font-weight: bold; letter-spacing: 5px; }
      fieldset { border: 1px solid #ddd; border-radius: 5px; margin-bottom: 15px; padding: 15px; }
      legend { font-weight: bold; color: #333; padding: 0 5px; }
      input[type="text"], input[type="password"], input[type="number"] { width: 100%; padding: 8px; margin: 8px 0; border: 1px solid #ccc; border-radius: 4px; box-sizing: border-box; }
      .row { display: flex; gap: 10px; }
      .row > div { flex: 1; }
      input[type="submit"] { background: #007bff; color: white; padding: 12px; border: none; border-radius: 4px; cursor: pointer; width: 100%; font-size: 16px; font-weight:bold; }
      input[type="submit"]:hover { background: #0056b3; }
      .chk-group { margin: 10px 0; display: flex; align-items: center; gap: 10px; }
    </style>
    <script>
      var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
      ws.onmessage = function(e) { document.getElementById('liveDisplay').innerText = e.data; };
    </script>
    </head><body>
      <div class="container">
        <h2>Dashboard</h2>
        <div class="live-view" id="liveDisplay">--:--</div>
        <form action="/save" method="POST">
          <fieldset><legend>Network</legend>
            <label>Primary SSID:</label><input type="text" name="ssid1" value="{SSID1}">
            <label>Primary Password:</label><input type="password" name="pass1" value="{PASS1}">
            <hr style="border:0; border-top:1px solid #eee;">
            <label>Secondary SSID (Fallback):</label><input type="text" name="ssid2" value="{SSID2}">
            <label>Secondary Password:</label><input type="password" name="pass2" value="{PASS2}">
          </fieldset>
          
          <fieldset><legend>Time</legend>
            <label>UTC Offset (sec):</label><input type="number" name="utc" value="{UTC}">
            <div class="chk-group"><input type="checkbox" name="dst" value="1" {DST}> <label>Daylight Saving (+1h)</label></div>
            <div class="chk-group"><input type="checkbox" name="u12h" value="1" {U12H}> <label>12-Hour Format (AM/PM)</label></div>
          </fieldset>
          
          <fieldset><legend>Brightness Schedule</legend>
            <div class="chk-group"><input type="checkbox" name="autobr" value="1" {AUTOBR}> <label>Enable Auto Schedule</label></div>
            <label>Manual Level (if auto is off):</label><input type="number" name="manbr" min="0" max="255" value="{MANBR}">
            
            <div class="row" style="margin-top:10px;">
              <div><b>Mode</b></div><div><b>Start Hour</b></div><div><b>Brightness</b></div>
            </div>
            <div class="row">
              <div>Day</div>
              <div><input type="number" name="h_day" min="0" max="23" value="{HDAY}"></div>
              <div><input type="number" name="b_day" min="0" max="255" value="{BDAY}"></div>
            </div>
            <div class="row">
              <div>Dusk</div>
              <div><input type="number" name="h_dusk" min="0" max="23" value="{HDUSK}"></div>
              <div><input type="number" name="b_dusk" min="0" max="255" value="{BDUSK}"></div>
            </div>
            <div class="row">
              <div>Night</div>
              <div><input type="number" name="h_night" min="0" max="23" value="{HNIGHT}"></div>
              <div><input type="number" name="b_night" min="0" max="255" value="{BNIGHT}"></div>
            </div>
          </fieldset>
          <input type="submit" value="Save Settings">
        </form>
      </div>
    </body></html>
    )rawliteral";

    html.replace("{SSID1}", wifi_ssid1); html.replace("{PASS1}", wifi_pass1);
    html.replace("{SSID2}", wifi_ssid2); html.replace("{PASS2}", wifi_pass2);
    html.replace("{UTC}", String(utcOffsetInSeconds));
    html.replace("{DST}", dstEnabled ? "checked" : "");
    html.replace("{U12H}", use12hFormat ? "checked" : "");
    html.replace("{AUTOBR}", autoBrightnessEnabled ? "checked" : "");
    html.replace("{MANBR}", String(manualBrightness));
    html.replace("{HDAY}", String(h_day)); html.replace("{BDAY}", String(b_day));
    html.replace("{HDUSK}", String(h_dusk)); html.replace("{BDUSK}", String(b_dusk));
    html.replace("{HNIGHT}", String(h_night)); html.replace("{BNIGHT}", String(b_night));

    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    // 1. Capture new WiFi settings
    String new_ssid1 = server.arg("ssid1");
    String new_pass1 = server.arg("pass1");
    String new_ssid2 = server.arg("ssid2");
    String new_pass2 = server.arg("pass2");

    // Check if WiFi actually changed
    bool wifiChanged = (new_ssid1 != wifi_ssid1 || new_pass1 != wifi_pass1 ||
                        new_ssid2 != wifi_ssid2 || new_pass2 != wifi_pass2);

    // 2. Save all to preferences
    preferences.putString("ssid1", new_ssid1);
    preferences.putString("pass1", new_pass1);
    preferences.putString("ssid2", new_ssid2);
    preferences.putString("pass2", new_pass2);
    
    preferences.putLong("utc", server.arg("utc").toInt());
    preferences.putBool("dst", server.hasArg("dst"));
    preferences.putBool("u12h", server.hasArg("u12h"));
    
    preferences.putBool("auto_br", server.hasArg("autobr"));
    preferences.putInt("man_br", server.arg("manbr").toInt());
    
    preferences.putInt("h_day", server.arg("h_day").toInt());
    preferences.putInt("b_day", server.arg("b_day").toInt());
    preferences.putInt("h_dusk", server.arg("h_dusk").toInt());
    preferences.putInt("b_dusk", server.arg("b_dusk").toInt());
    preferences.putInt("h_night", server.arg("h_night").toInt());
    preferences.putInt("b_night", server.arg("b_night").toInt());
    
    preferences.putInt("fails", 0);

    // 3. Update Global Variables INSTANTLY
    wifi_ssid1 = new_ssid1;
    wifi_pass1 = new_pass1;
    wifi_ssid2 = new_ssid2;
    wifi_pass2 = new_pass2;
    utcOffsetInSeconds = server.arg("utc").toInt();
    dstEnabled = server.hasArg("dst");
    use12hFormat = server.hasArg("u12h");
    autoBrightnessEnabled = server.hasArg("autobr");
    manualBrightness = server.arg("manbr").toInt();
    h_day = server.arg("h_day").toInt();
    b_day = server.arg("b_day").toInt();
    h_dusk = server.arg("h_dusk").toInt();
    b_dusk = server.arg("b_dusk").toInt();
    h_night = server.arg("h_night").toInt();
    b_night = server.arg("b_night").toInt();

    // 4. Apply Time & Brightness Changes Live
    applyNtpOffset();
    if (autoBrightnessEnabled) {
      lastHourChecked = -1; // Forces the loop to recalculate auto-brightness immediately
    } else {
      analogWrite(DIGITS_VCC, manualBrightness);
    }

    // 5. Respond to browser based on whether a reboot is needed
    if (wifiChanged) {
      server.send(200, "text/html", "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:Arial;text-align:center;padding:50px;background:#f4f4f4;}</style></head><body><h2>WiFi Changed!</h2><p>Rebooting to connect to new network...</p></body></html>");
      delay(1000);
      ESP.restart();
    } else {
      // If WiFi didn't change, show a success message and auto-redirect back to the dashboard in 1 second
      server.send(200, "text/html", "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><meta http-equiv='refresh' content='1;url=/'><style>body{font-family:Arial;text-align:center;padding:50px;background:#d4edda;color:#155724;}</style></head><body><h2>Settings Applied Live!</h2><p>Returning to dashboard...</p></body></html>");
    }
  });
}

// =======================================================
// ==================== DISPLAY & LOGIC ==================
// =======================================================

void setAutoBrightness() {
  int targetB = b_day; 
  if (h_night > h_day) {
    if (hh >= h_night || hh < h_day) targetB = b_night;
    else if (hh >= h_dusk) targetB = b_dusk;
  } else { // Night spans midnight (e.g., 22 to 07)
    if (hh >= h_night || hh < h_day) targetB = b_night;
    else if (hh >= h_dusk && hh < h_night) targetB = b_dusk;
  }
  analogWrite(DIGITS_VCC, targetB);
}

void displayTime(int hours, int minutes, bool colonOn) {
  int dispHours = hours;
  if (use12hFormat) {
    dispHours = hours % 12;
    if (dispHours == 0) dispHours = 12;
  }
  
  dispHours = constrain(dispHours, 0, 23);
  minutes = constrain(minutes, 0, 59);
  int digits[4] = {minutes % 10, (minutes / 10) % 10, dispHours % 10, (dispHours / 10) % 10};
  if (digits[3] == 0) digits[3] = -2; 
  
  updateDisplay(getPattern(digits[3], colonOn), getPattern(digits[2], false), getPattern(digits[1], false), getPattern(digits[0], false));
  
  // Format for WebSocket
  char buf[6];
  sprintf(buf, "%s%d%c%02d", (digits[3] == -2) ? " " : String(digits[3]).c_str(), digits[2], colonOn ? ':' : ' ', minutes);
  currentDisplayStr = String(buf);
}

void displayInteger(int num) {
  num = constrain(num, -999, 9999);
  currentDisplayStr = String(num); // For WebSocket
  bool isNegative = num < 0;
  num = abs(num);
  int digits[4] = {num % 10, (num / 10) % 10, (num / 100) % 10, (num / 1000) % 10};
  bool nonZeroFound = false;
  for (int pos = 3; pos > 0; pos--) {
    if (digits[pos] == 0 && !nonZeroFound) digits[pos] = -2;
    else nonZeroFound = true;
  }
  if (isNegative) {
    if (digits[3] == -2) digits[3] = -1;
    else if (digits[2] == -2) digits[2] = -1;
  }
  updateDisplay(getPattern(digits[3], false), getPattern(digits[2], false), getPattern(digits[1], false), getPattern(digits[0], false));
}

void displayFloat(float num) { displayTemperature(num); }

void displayTemperature(float temp) {
  if (isnan(temp) || temp > 999.9 || temp < -99.9) return;
  
  char buf[6];
  dtostrf(temp, 4, 1, buf);
  currentDisplayStr = String(buf); // For WebSocket

  bool isNegative = (temp < 0);
  int value = (int)round(fabs(temp) * 10);
  int digits[4] = {value % 10, (value / 10) % 10, (value / 100) % 10, (value / 1000) % 10};

  for (int pos = 3; pos > 1; pos--) {
    if (digits[pos] == 0) digits[pos] = -2;
    else break;
  }
  if (isNegative) {
    for (int pos = 3; pos >= 0; pos--) {
      if (digits[pos] == -2) { digits[pos] = -1; break; }
    }
  }
  updateDisplay(getPattern(digits[3], false), getPattern(digits[2], false), getPattern(digits[1], true), getPattern(digits[0], false));
}

void displayAnimationStep(int step) {
  if (step < 0 || step >= NUM_ANIM_STEPS) return;
  updateDisplay(animationFrames[step][0], animationFrames[step][1], animationFrames[step][2], animationFrames[step][3]);
}

byte getPattern(int digit, bool dpOn) {
  byte pattern = (digit >= 0 && digit <= 9) ? digitPatterns_anode[digit] : (digit == -1 ? MINUS_PATTERN : BLANK_PATTERN);
  if (dpOn) pattern |= 0x80;
  return pattern;
}

void updateDisplay(byte pattern4, byte pattern3, byte pattern2, byte pattern1) {
  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern1);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern2);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern3);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern4);
  digitalWrite(LATCH_PIN, HIGH);
}

void handleUdpCommands() {
  int packetSize = udpListener.parsePacket();
  if (packetSize) {
    int len = udpListener.read(packetBuffer, 255);
    if (len > 0) packetBuffer[len] = 0; 
    String input = String(packetBuffer); input.trim();
    if (input.length() > 0) {
      char cmd = input.charAt(0); String arg = input.substring(1); arg.trim();
      if (cmd == 't') { displayTemperature(arg.toFloat()); currentMode = OVERRIDE; overrideStartTime = millis(); }
    }
  }
}