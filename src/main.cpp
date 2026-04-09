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
int targetBrightness = 255;
int displayStyle = 0;
int transitionStyle = 0;

// Hourly Effects Mask (24 hours). Bit 0: Spin, Bit 1: Cylon, Bit 2: Snake, Bit 3: Glitch, Bit 4: Flash, Bit 5: Slot
uint8_t hourlyEffects[24];

// --- WIFI, WEB SERVER & WEBSOCKETS ---
ESP8266WebServer server(80);
WebSocketsServer webSocket(81);
bool isAPMode = false;
String currentDisplayStr = "";
String lastDisplayStr = "";
unsigned long lastWsUpdate = 0;

// --- ADVANCED ANIMATION DATA ---
const int NUM_ANIM_STEPS = 28;
const byte animationFrames[NUM_ANIM_STEPS][4] = {
    {0b00000001, 0, 0, 0}, {0b00100000, 0, 0, 0}, {0b00010000, 0, 0, 0}, {0b00001000, 0, 0, 0}, {0, 0b00010000, 0, 0}, {0, 0b00100000, 0, 0}, {0, 0b00000001, 0, 0}, {0, 0b00000010, 0, 0}, {0, 0b00000100, 0, 0}, {0, 0b10000000, 0, 0}, {0, 0, 0b00010000, 0}, {0, 0, 0b00100000, 0}, {0, 0, 0b00000001, 0}, {0, 0, 0b00000010, 0}, {0, 0, 0b00000100, 0}, {0, 0, 0b10000000, 0}, {0, 0, 0, 0b00010000}, {0, 0, 0, 0b00100000}, {0, 0, 0, 0b00000001}, {0, 0, 0, 0b00000010}, {0, 0, 0, 0b00000100}, {0, 0, 0, 0b00001000}, {0, 0, 0b10000000, 0}, {0, 0, 0b00001000, 0}, {0, 0b10000000, 0, 0}, {0, 0b00001000, 0, 0}, {0b00000100, 0, 0, 0}, {0b00000010, 0, 0, 0}};

const byte cylonFrames[6][4] = {
    {0x40, 0, 0, 0}, {0, 0x40, 0, 0}, {0, 0, 0x40, 0}, {0, 0, 0, 0x40}, {0, 0, 0x40, 0}, {0, 0x40, 0, 0}};

const byte snakeFrames[12][4] = {
    {0x01, 0, 0, 0}, {0, 0x01, 0, 0}, {0, 0, 0x01, 0}, {0, 0, 0, 0x01}, {0, 0, 0, 0x02}, {0, 0, 0, 0x04}, {0, 0, 0, 0x08}, {0, 0, 0x08, 0}, {0, 0x08, 0, 0}, {0x08, 0, 0, 0}, {0x10, 0, 0, 0}, {0x20, 0, 0, 0}};

const byte CONF[4] = {0b01011000, 0b01011100, 0b01010100, 0b01110001};

// --- STATE MANAGEMENT ---
enum DisplayMode
{
  IDLE_TIME,
  OVERRIDE,
  ANIMATION,
  GLITCH,
  FLASH,
  CYLON,
  SNAKE,
  SLOT_MACHINE
};
DisplayMode currentMode = IDLE_TIME;
unsigned long overrideStartTime = 0;
unsigned long OVERRIDE_DURATION = 10000;
unsigned long lastEffectUpdate = 0;

int hh = 0, mm = 0, ss = 0;
int lastHourChecked = -1;
uint32_t targetTime = 0;
bool timeSuccessfullySet = false;
unsigned long lastNtpAttempt = 0;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// --- PROTOTYPES ---
void loadSettings();
void setupWebServerRoutes();
void applyNtpOffset();
void calculateBrightness();
byte getPattern(int digit, bool dpOn);
void updateDisplay(byte p4, byte p3, byte p2, byte p1);
void displayTime(int hours, int minutes, bool colonOn);
bool connectToWiFi(String ssid, String pass, int timeoutSec);

// =======================================================
// ==================== SETUP ============================
// =======================================================
void setup()
{
  Serial.begin(115200);
  pinMode(LATCH_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(DATA_PIN, OUTPUT);
  pinMode(DIGITS_VCC, OUTPUT);

  loadSettings();
  calculateBrightness();

  int bootFails = preferences.getInt("fails", 0);
  bootFails++;
  preferences.putInt("fails", bootFails);

  analogWrite(DIGITS_VCC, targetBrightness);
  updateDisplay(CONF[0], CONF[1], CONF[2], CONF[3]);
  currentDisplayStr = "CONF";
  delay(1000);

  if (bootFails >= 5)
  {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Clock_Config");
    isAPMode = true;
    preferences.putInt("fails", 0);
  }
  else
  {
    WiFi.mode(WIFI_STA);
    bool connected = connectToWiFi(wifi_ssid1, wifi_pass1, 10);
    if (!connected && wifi_ssid2.length() > 0)
    {
      connected = connectToWiFi(wifi_ssid2, wifi_pass2, 10);
    }

    if (!connected)
    {
      delay(1000);
      ESP.restart();
    }

    preferences.putInt("fails", 0);
    ArduinoOTA.setHostname("esp8266_CLOCK2");
    ArduinoOTA.setPassword("admin");
    ArduinoOTA.begin();

    timeClient.begin();
    applyNtpOffset();
    if (timeClient.forceUpdate())
    {
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

  if (!isAPMode)
  {
    lastHourChecked = hh;
    targetTime = millis();
  }
}

// =======================================================
// ==================== LOOP =============================
// =======================================================
void loop()
{
  server.handleClient();
  webSocket.loop();

  if (currentDisplayStr != lastDisplayStr && millis() - lastWsUpdate > 250)
  {
    webSocket.broadcastTXT(currentDisplayStr);
    lastDisplayStr = currentDisplayStr;
    lastWsUpdate = millis();
  }

  if (!isAPMode)
  {
    unsigned long currentMillis = millis();
    unsigned long ntpInterval = timeSuccessfullySet ? 3600000UL : 30000UL;

    if (WiFi.status() == WL_CONNECTED && (currentMillis - lastNtpAttempt > ntpInterval))
    {
      if (timeClient.update())
      {
        hh = timeClient.getHours();
        mm = timeClient.getMinutes();
        ss = timeClient.getSeconds();
        timeSuccessfullySet = true;
      }
      lastNtpAttempt = currentMillis;
    }

    ArduinoOTA.handle();

    // -- TIMEKEEPING & EFFECTS --
    if (targetTime < millis())
    {
      targetTime += 1000;
      ss++;
      if (ss > 59)
      {
        ss = 0;
        mm++;
        if (mm > 59)
        {
          mm = 0;
          hh++;
          if (hh > 23)
            hh = 0;

          // --- HOURLY TRIGGER LOGIC ---
          uint8_t mask = hourlyEffects[hh];
          if (mask > 0)
          {
            int activeFx[6];
            int fxCount = 0;
            for (int i = 0; i < 6; i++)
            {
              if (mask & (1 << i))
                activeFx[fxCount++] = i;
            }
            if (fxCount > 0)
            {
              int chosenFx = activeFx[random(0, fxCount)]; // Pick random if multiple checked
              DisplayMode mappedFx[6] = {ANIMATION, CYLON, SNAKE, GLITCH, FLASH, SLOT_MACHINE};
              currentMode = mappedFx[chosenFx];
              overrideStartTime = millis();
              OVERRIDE_DURATION = 10000; // Plays for 10 seconds
            }
          }
        }
        else if (transitionStyle == 1 && currentMode == IDLE_TIME)
        {
          // Normal minute transition if top-of-hour effect isn't playing
          currentMode = SLOT_MACHINE;
          overrideStartTime = millis();
          OVERRIDE_DURATION = 1500;
        }
      }
    }

    if (hh != lastHourChecked)
    {
      calculateBrightness();
      lastHourChecked = hh;
    }

    // -- DYNAMIC BRIGHTNESS --
    static int currentPWM = -1;
    int desiredPWM;
    if (displayStyle == 1 && currentMode == IDLE_TIME)
    {
      float wave = (sin(millis() / 600.0) + 1.0) / 2.0;
      int minB = (targetBrightness > 10) ? targetBrightness / 5 : 1;
      desiredPWM = minB + (wave * (targetBrightness - minB));
    }
    else
    {
      desiredPWM = targetBrightness;
    }
    if (desiredPWM != currentPWM)
    {
      analogWrite(DIGITS_VCC, desiredPWM);
      currentPWM = desiredPWM;
    }

    // -- RENDER MODES --
    if ((currentMode != IDLE_TIME) && (millis() - overrideStartTime > OVERRIDE_DURATION))
    {
      currentMode = IDLE_TIME;
    }

    switch (currentMode)
    {
    case IDLE_TIME:
      displayTime(hh, mm, (ss % 2 == 0));
      break;
    case SLOT_MACHINE:
    {
      if (millis() - lastEffectUpdate > 50)
      {
        int randMin1 = random(0, 10);
        int randMin2 = random(0, 10);
        int dispHours = use12hFormat ? (hh % 12 == 0 ? 12 : hh % 12) : hh;
        int h1 = dispHours % 10;
        int h2 = (dispHours / 10) % 10;
        if (h2 == 0)
          h2 = -2;
        updateDisplay(getPattern(h2, false), getPattern(h1, true), getPattern(randMin2, false), getPattern(randMin1, false));
        currentDisplayStr = "ROLL";
        lastEffectUpdate = millis();
      }
      break;
    }
    case ANIMATION:
    {
      int step = ((millis() - overrideStartTime) / 50) % NUM_ANIM_STEPS;
      updateDisplay(animationFrames[step][0], animationFrames[step][1], animationFrames[step][2], animationFrames[step][3]);
      currentDisplayStr = "SPIN";
      break;
    }
    case CYLON:
    {
      int step = ((millis() - overrideStartTime) / 100) % 6;
      updateDisplay(cylonFrames[step][0], cylonFrames[step][1], cylonFrames[step][2], cylonFrames[step][3]);
      currentDisplayStr = "CYLN";
      break;
    }
    case SNAKE:
    {
      int step = ((millis() - overrideStartTime) / 80) % 12;
      updateDisplay(snakeFrames[step][0], snakeFrames[step][1], snakeFrames[step][2], snakeFrames[step][3]);
      currentDisplayStr = "SNAK";
      break;
    }
    case FLASH:
    {
      if ((millis() / 200) % 2 == 0)
        displayTime(hh, mm, true);
      else
        updateDisplay(0, 0, 0, 0);
      currentDisplayStr = "FLSH";
      break;
    }
    case GLITCH:
    {
      if (millis() - lastEffectUpdate > 30)
      {
        updateDisplay(random(0, 255), random(0, 255), random(0, 255), random(0, 255));
        lastEffectUpdate = millis();
      }
      currentDisplayStr = "GLCH";
      break;
    }
    case OVERRIDE:
      break;
    }
  }
}

// =======================================================
// ==================== SETTINGS & WEB ===================
// =======================================================

bool connectToWiFi(String ssid, String pass, int timeoutSec)
{
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long timeout = millis() + (timeoutSec * 1000);
  int animStep = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() > timeout)
      return false;
    updateDisplay(animationFrames[animStep][0], animationFrames[animStep][1], animationFrames[animStep][2], animationFrames[animStep][3]);
    animStep = (animStep + 1) % NUM_ANIM_STEPS;
    delay(50);
  }
  return true;
}

void loadSettings()
{
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
  displayStyle = preferences.getInt("style", 0);
  transitionStyle = preferences.getInt("trans", 0);

  if (preferences.getBytesLength("fxMasks") == 24)
  {
    preferences.getBytes("fxMasks", hourlyEffects, 24);
  }
  else
  {
    memset(hourlyEffects, 0, 24);
  }
}

void applyNtpOffset()
{
  timeClient.setTimeOffset(utcOffsetInSeconds + (dstEnabled ? 3600 : 0));
}

void setupWebServerRoutes()
{
  server.on("/", HTTP_GET, []()
            {
    String html = R"rawliteral(
    <!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Clock Dashboard</title>
    <style>
      :root { --bg: #121212; --panel: #1e1e1e; --text: #e0e0e0; --accent: #bb86fc; --success: #03dac6; --danger: #cf6679; --border: #333; }
      body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 20px 10px; background: var(--bg); color: var(--text); }
      .container { background: var(--panel); padding: 20px; border-radius: 12px; max-width: 600px; margin: auto; box-shadow: 0 8px 24px rgba(0,0,0,0.6); }
      h2 { color: #fff; text-align: center; margin-top: 0; }
      .live-view { background: #000; color: #0f0; font-family: 'Courier New', monospace; font-size: 3.5rem; text-align: center; padding: 15px; border-radius: 8px; margin-bottom: 25px; font-weight: bold; letter-spacing: 4px; box-shadow: inset 0 0 15px #0f04; border: 2px solid #0f02; }
      fieldset { border: 1px solid var(--border); border-radius: 8px; margin-bottom: 20px; padding: 15px; background: #232323; }
      legend { font-weight: 600; color: var(--accent); padding: 0 8px; background: var(--panel); border-radius: 4px; border: 1px solid var(--border); }
      input[type="text"], input[type="password"], input[type="number"], select { width: 100%; padding: 10px; margin: 8px 0 15px; border: 1px solid var(--border); border-radius: 6px; box-sizing: border-box; background: #2c2c2c; color: #fff; font-size: 16px; }
      input[type="text"]:focus, input[type="number"]:focus { outline: 2px solid var(--accent); }
      .row { display: flex; gap: 15px; } .row > div { flex: 1; }
      input[type="submit"] { background: var(--accent); color: #000; padding: 14px; border: none; border-radius: 6px; cursor: pointer; width: 100%; font-size: 18px; font-weight: bold; text-transform: uppercase; letter-spacing: 1px; transition: 0.2s; }
      input[type="submit"]:active { transform: scale(0.98); }
      .btn-fx { background: #333; color: white; padding: 10px; border: 1px solid var(--border); border-radius: 6px; cursor: pointer; flex: 1 1 30%; font-size: 14px; font-weight: 600; margin-bottom: 10px; }
      .btn-fx:active { background: #444; }
      .chk-group { margin: 12px 0; display: flex; align-items: center; gap: 12px; font-size: 16px; }
      input[type="checkbox"] { width: 20px; height: 20px; accent-color: var(--accent); }
      table.fx-grid { width: 100%; border-collapse: collapse; margin-top: 10px; text-align: center; }
      table.fx-grid th, table.fx-grid td { padding: 8px 4px; border: 1px solid var(--border); }
      table.fx-grid th { background: #333; font-size: 12px; color: #aaa; }
      table.fx-grid td:first-child { font-weight: bold; background: #2a2a2a; color: var(--accent); }
      .grid-wrapper { max-height: 400px; overflow-y: auto; border: 1px solid var(--border); border-radius: 6px; }
    </style>
    <script>
      var ws = new WebSocket('ws://' + window.location.hostname + ':81/');
      ws.onmessage = function(e) { document.getElementById('liveDisplay').innerText = e.data; };
      function triggerFx(fx) { fetch('/trigger?fx=' + fx); }
      
      // Build 2D grid dynamically to save ESP memory
      document.addEventListener("DOMContentLoaded", function() {
        const savedMasks = {FX_MASKS}; // e.g. [0, 5, 0...]
        const tbody = document.getElementById('fxTableBody');
        for(let h=0; h<24; h++) {
          let tr = document.createElement('tr');
          tr.innerHTML = `<td>${h.toString().padStart(2, '0')}:00</td>`;
          for(let f=0; f<6; f++) {
            let checked = (savedMasks[h] & (1<<f)) ? 'checked' : '';
            tr.innerHTML += `<td><input type="checkbox" class="fx-cb" data-h="${h}" data-f="${f}" ${checked}></td>`;
          }
          tbody.appendChild(tr);
        }
      });

      function prepSubmit() {
        let masks = new Array(24).fill(0);
        document.querySelectorAll('.fx-cb:checked').forEach(cb => {
          masks[parseInt(cb.dataset.h)] |= (1 << parseInt(cb.dataset.f));
        });
        document.getElementById('fx_data').value = masks.join(',');
        return true;
      }
    </script>
    </head><body>
      <div class="container">
        <h2>Dashboard</h2>
        <div class="live-view" id="liveDisplay">--:--</div>
        
        <fieldset><legend>Live Effects Console</legend>
          <div style="display:flex; justify-content:space-between; flex-wrap:wrap; gap:5px;">
            <button class="btn-fx" onclick="triggerFx('spin')">▶ Spin</button>
            <button class="btn-fx" onclick="triggerFx('cylon')">▀ Cylon</button>
            <button class="btn-fx" onclick="triggerFx('snake')">◿ Snake</button>
            <button class="btn-fx" style="border-color:var(--danger);" onclick="triggerFx('glitch')">👾 Glitch</button>
            <button class="btn-fx" style="border-color:orange;" onclick="triggerFx('flash')">⚡ Flash</button>
            <button class="btn-fx" onclick="triggerFx('slot')">🎰 Slot</button>
          </div>
        </fieldset>

        <form action="/save" method="POST" onsubmit="return prepSubmit()">
          
          <fieldset><legend>Hourly Auto-Effects Configurator</legend>
            <p style="font-size:13px; color:#aaa; margin-top:0;">Check boxes to trigger an effect exactly when the hour changes. Checking multiple boxes for one hour triggers "Auto Mode" (randomly picks one of your checked effects).</p>
            <input type="hidden" name="fx_data" id="fx_data">
            <div class="grid-wrapper">
              <table class="fx-grid">
                <thead><tr><th>Hour</th><th>Spin</th><th>Cyln</th><th>Snak</th><th>Glch</th><th>Flsh</th><th>Slot</th></tr></thead>
                <tbody id="fxTableBody"></tbody>
              </table>
            </div>
          </fieldset>

          <fieldset><legend>Network Options</legend>
            <label>Primary SSID:</label><input type="text" name="ssid1" value="{SSID1}">
            <label>Primary Password:</label><input type="password" name="pass1" value="{PASS1}">
            <hr style="border:0; border-top:1px solid var(--border); margin:15px 0;">
            <label>Secondary SSID (Fallback):</label><input type="text" name="ssid2" value="{SSID2}">
            <label>Secondary Password:</label><input type="password" name="pass2" value="{PASS2}">
          </fieldset>
          
          <fieldset><legend>Time & Display Formatting</legend>
            <label>UTC Offset (sec):</label><input type="number" name="utc" value="{UTC}">
            <div class="chk-group"><input type="checkbox" name="dst" value="1" {DST}> <label>Daylight Saving (+1h)</label></div>
            <div class="chk-group"><input type="checkbox" name="u12h" value="1" {U12H}> <label>12-Hour Format (AM/PM)</label></div>
            
            <label>Idle Style:</label>
            <select name="style">
              <option value="0" {S_SEL0}>Solid</option>
              <option value="1" {S_SEL1}>Breathing Pulse</option>
            </select>
            <label>Standard Minute Transition:</label>
            <select name="trans">
              <option value="0" {T_SEL0}>Instant Snap</option>
              <option value="1" {T_SEL1}>Slot Machine Roll</option>
            </select>
          </fieldset>
          
          <fieldset><legend>Brightness Schedule</legend>
            <div class="chk-group"><input type="checkbox" name="autobr" value="1" {AUTOBR}> <label>Enable Auto Schedule</label></div>
            <label>Manual Level (if auto is off):</label><input type="number" name="manbr" min="0" max="255" value="{MANBR}">
            
            <div class="row" style="margin-top:10px;"><div><b>Mode</b></div><div><b>Start Hr</b></div><div><b>Level (0-255)</b></div></div>
            <div class="row"><div>Day</div><div><input type="number" name="h_day" min="0" max="23" value="{HDAY}"></div><div><input type="number" name="b_day" min="0" max="255" value="{BDAY}"></div></div>
            <div class="row"><div>Dusk</div><div><input type="number" name="h_dusk" min="0" max="23" value="{HDUSK}"></div><div><input type="number" name="b_dusk" min="0" max="255" value="{BDUSK}"></div></div>
            <div class="row"><div>Night</div><div><input type="number" name="h_night" min="0" max="23" value="{HNIGHT}"></div><div><input type="number" name="b_night" min="0" max="255" value="{BNIGHT}"></div></div>
          </fieldset>
          <input type="submit" value="Save Settings">
        </form>
      </div>
    </body></html>
    )rawliteral";

    html.replace("{SSID1}", wifi_ssid1); html.replace("{PASS1}", wifi_pass1);
    html.replace("{SSID2}", wifi_ssid2); html.replace("{PASS2}", wifi_pass2);
    html.replace("{UTC}", String(utcOffsetInSeconds));
    html.replace("{DST}", dstEnabled ? "checked" : ""); html.replace("{U12H}", use12hFormat ? "checked" : "");
    html.replace("{AUTOBR}", autoBrightnessEnabled ? "checked" : ""); html.replace("{MANBR}", String(manualBrightness));
    html.replace("{HDAY}", String(h_day)); html.replace("{BDAY}", String(b_day));
    html.replace("{HDUSK}", String(h_dusk)); html.replace("{BDUSK}", String(b_dusk));
    html.replace("{HNIGHT}", String(h_night)); html.replace("{BNIGHT}", String(b_night));
    html.replace("{S_SEL0}", displayStyle == 0 ? "selected" : ""); html.replace("{S_SEL1}", displayStyle == 1 ? "selected" : "");
    html.replace("{T_SEL0}", transitionStyle == 0 ? "selected" : ""); html.replace("{T_SEL1}", transitionStyle == 1 ? "selected" : "");

    // Generate JSON array string for the masks
    String masksStr = "[";
    for(int i=0; i<24; i++) { masksStr += String(hourlyEffects[i]) + (i<23 ? "," : ""); }
    masksStr += "]";
    html.replace("{FX_MASKS}", masksStr);

    server.send(200, "text/html", html); });

  server.on("/trigger", HTTP_GET, []()
            {
    String fx = server.arg("fx");
    overrideStartTime = millis(); OVERRIDE_DURATION = 10000; 
    if (fx == "spin") currentMode = ANIMATION;
    else if (fx == "cylon") currentMode = CYLON;
    else if (fx == "snake") currentMode = SNAKE;
    else if (fx == "glitch") currentMode = GLITCH;
    else if (fx == "flash") currentMode = FLASH;
    else if (fx == "slot") { currentMode = SLOT_MACHINE; OVERRIDE_DURATION = 3000; }
    server.send(200, "text/plain", "OK"); });

  server.on("/save", HTTP_POST, []()
            {
    String new_ssid1 = server.arg("ssid1"); String new_pass1 = server.arg("pass1");
    String new_ssid2 = server.arg("ssid2"); String new_pass2 = server.arg("pass2");
    bool wifiChanged = (new_ssid1 != wifi_ssid1 || new_pass1 != wifi_pass1 || new_ssid2 != wifi_ssid2 || new_pass2 != wifi_pass2);

    preferences.putString("ssid1", new_ssid1); preferences.putString("pass1", new_pass1);
    preferences.putString("ssid2", new_ssid2); preferences.putString("pass2", new_pass2);
    preferences.putLong("utc", server.arg("utc").toInt());
    preferences.putBool("dst", server.hasArg("dst")); preferences.putBool("u12h", server.hasArg("u12h"));
    preferences.putBool("auto_br", server.hasArg("autobr")); preferences.putInt("man_br", server.arg("manbr").toInt());
    preferences.putInt("h_day", server.arg("h_day").toInt()); preferences.putInt("b_day", server.arg("b_day").toInt());
    preferences.putInt("h_dusk", server.arg("h_dusk").toInt()); preferences.putInt("b_dusk", server.arg("b_dusk").toInt());
    preferences.putInt("h_night", server.arg("h_night").toInt()); preferences.putInt("b_night", server.arg("b_night").toInt());
    preferences.putInt("style", server.arg("style").toInt()); preferences.putInt("trans", server.arg("trans").toInt());
    
    // Parse the fx_data (comma separated integers)
    String fxStr = server.arg("fx_data");
    int start = 0;
    for(int i=0; i<24; i++) {
      int idx = fxStr.indexOf(',', start);
      if (idx == -1) idx = fxStr.length();
      hourlyEffects[i] = fxStr.substring(start, idx).toInt();
      start = idx + 1;
    }
    preferences.putBytes("fxMasks", hourlyEffects, 24);
    preferences.putInt("fails", 0);

    wifi_ssid1 = new_ssid1; wifi_pass1 = new_pass1; wifi_ssid2 = new_ssid2; wifi_pass2 = new_pass2;
    utcOffsetInSeconds = server.arg("utc").toInt(); dstEnabled = server.hasArg("dst"); use12hFormat = server.hasArg("u12h");
    autoBrightnessEnabled = server.hasArg("autobr"); manualBrightness = server.arg("manbr").toInt();
    h_day = server.arg("h_day").toInt(); b_day = server.arg("b_day").toInt();
    h_dusk = server.arg("h_dusk").toInt(); b_dusk = server.arg("b_dusk").toInt();
    h_night = server.arg("h_night").toInt(); b_night = server.arg("b_night").toInt();
    displayStyle = server.arg("style").toInt(); transitionStyle = server.arg("trans").toInt();

    applyNtpOffset();
    if (autoBrightnessEnabled) lastHourChecked = -1; else targetBrightness = manualBrightness;

    if (wifiChanged) {
      server.send(200, "text/html", "<html><body style='background:#121212; color:#fff; text-align:center; padding:50px; font-family:sans-serif;'><h2>WiFi Changed!</h2><p>Rebooting...</p></body></html>");
      delay(1000); ESP.restart();
    } else {
      server.send(200, "text/html", "<html><head><meta http-equiv='refresh' content='1;url=/'></head><body style='background:#121212; color:#03dac6; text-align:center; padding:50px; font-family:sans-serif;'><h2>Settings Applied Live!</h2><p>Returning to dashboard...</p></body></html>");
    } });
}

// =======================================================
// ==================== DISPLAY HELPER ===================
// =======================================================

void calculateBrightness()
{
  if (!autoBrightnessEnabled)
  {
    targetBrightness = manualBrightness;
    return;
  }
  int targetB = b_day;
  if (h_night > h_day)
  {
    if (hh >= h_night || hh < h_day)
      targetB = b_night;
    else if (hh >= h_dusk)
      targetB = b_dusk;
  }
  else
  {
    if (hh >= h_night || hh < h_day)
      targetB = b_night;
    else if (hh >= h_dusk && hh < h_night)
      targetB = b_dusk;
  }
  targetBrightness = targetB;
}

void displayTime(int hours, int minutes, bool colonOn)
{
  int dispHours = hours;
  if (use12hFormat)
  {
    dispHours = hours % 12;
    if (dispHours == 0)
      dispHours = 12;
  }
  dispHours = constrain(dispHours, 0, 23);
  minutes = constrain(minutes, 0, 59);
  int digits[4] = {minutes % 10, (minutes / 10) % 10, dispHours % 10, (dispHours / 10) % 10};
  if (digits[3] == 0)
    digits[3] = -2;

  updateDisplay(getPattern(digits[3], colonOn), getPattern(digits[2], false), getPattern(digits[1], false), getPattern(digits[0], false));

  char buf[6];
  sprintf(buf, "%s%d%c%02d", (digits[3] == -2) ? " " : String(digits[3]).c_str(), digits[2], colonOn ? ':' : ' ', minutes);
  currentDisplayStr = String(buf);
}

byte getPattern(int digit, bool dpOn)
{
  byte pattern = (digit >= 0 && digit <= 9) ? digitPatterns_anode[digit] : (digit == -1 ? MINUS_PATTERN : BLANK_PATTERN);
  if (dpOn)
    pattern |= 0x80;
  return pattern;
}

void updateDisplay(byte pattern4, byte pattern3, byte pattern2, byte pattern1)
{
  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern1);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern2);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern3);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern4);
  digitalWrite(LATCH_PIN, HIGH);
}