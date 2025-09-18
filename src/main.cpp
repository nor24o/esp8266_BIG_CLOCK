#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <math.h>
#include <stdarg.h>

#include <NTPClient.h>
// WiFiUdp is already included above

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

// --- WIFI & NTP ---
#ifndef STASSID
#define STASSID "Link2"
#define STAPSK "Horvat2017"
#endif
const char *ssid = STASSID;
const char *password = STAPSK;
const long utcOffsetInSeconds = 3 * 3600;

// --- ANIMATION DATA ---
const int NUM_ANIM_STEPS = 28;
const byte animationFrames[NUM_ANIM_STEPS][4] = {
    // Digit 4 (Leftmost)
    {0b00000001, 0, 0, 0},
    {0b00100000, 0, 0, 0},
    {0b00010000, 0, 0, 0},
    {0b00001000, 0, 0, 0},
    {0, 0b00010000, 0, 0},
    {0, 0b00100000, 0, 0},
    {0, 0b00000001, 0, 0},
    // Digit 3
    {0, 0b00000010, 0, 0},
    {0, 0b00000100, 0, 0},
    {0, 0b10000000, 0, 0},
    {0, 0, 0b00010000, 0},
    {0, 0, 0b00100000, 0},
    {0, 0, 0b00000001, 0},
    {0, 0, 0b00000010, 0},
    // Digit 2
    {0, 0, 0b00000100, 0},
    {0, 0, 0b10000000, 0},
    {0, 0, 0, 0b00010000},
    {0, 0, 0, 0b00100000},
    {0, 0, 0, 0b00000001},
    {0, 0, 0, 0b00000010},
    {0, 0, 0, 0b00000100},
    // Digit 1 (Rightmost)
    {0, 0, 0, 0b00001000},
    {0, 0, 0b10000000, 0},
    {0, 0, 0b00001000, 0},
    {0, 0b10000000, 0, 0},
    {0, 0b00001000, 0, 0},
    {0b00000100, 0, 0, 0},
    {0b00000010, 0, 0, 0}};

const byte CONF[4] = {0b01011000, 0b01011100, 0b01010100, 0b01110001}; // C O n F

// --- BRIGHTNESS SETTINGS ---
const int BRIGHTNESS_DAY = 255;
const int BRIGHTNESS_DUSK = 50;
const int BRIGHTNESS_NIGHT = 3;
bool autoBrightnessEnabled = true;
int lastHourChecked = -1;

// --- STATE MANAGEMENT ---
enum DisplayMode
{
  IDLE_TIME,
  OVERRIDE,
  ANIMATION
};
DisplayMode currentMode = IDLE_TIME;
unsigned long overrideStartTime = 0;
const unsigned long OVERRIDE_DURATION = 20000; // 20 seconds

// --- TIME VARIABLES ---
int hh = 0, mm = 0, ss = 0;
uint32_t targetTime = 0;

// --- NETWORKING OBJECTS ---
WiFiServer telnetServer(23);
WiFiClient telnetClient;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

// *** NEW: UDP listener for commands ***
const unsigned int UDP_PORT = 4210; // Port to listen on for UDP commands
WiFiUDP udpListener;
char packetBuffer[255]; // Buffer to hold incoming UDP packet data

// --- LOGGING PROXY (Unchanged) ---
class SerialMirror : public Print
{
public:
  size_t write(uint8_t c) override
  {
    Serial.write(c);
    if (telnetClient && telnetClient.connected())
    {
      telnetClient.write(c);
    }
    return 1;
  }
  void printf(const char *format, ...)
  {
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
void OtaConfig();
void setAutoBrightness();
void displayAnimationStep(int step);
byte getPattern(int digit, bool dpOn);
void updateDisplay(byte p4, byte p3, byte p2, byte p1);
void displayFloat(float num);
void displayInteger(int num);
void displayTemperature(float temp);
void displayTime(int hours, int minutes, bool colonOn);
void showLastIPPart();
void handleUdpCommands(); // *** NEW: Function to handle UDP packet parsing

void setup()
{
  Serial.begin(115200);
  LOG_PRINTLN("Booting");

  pinMode(LATCH_PIN, OUTPUT);
  pinMode(CLOCK_PIN, OUTPUT);
  pinMode(DATA_PIN, OUTPUT);
  pinMode(DIGITS_VCC, OUTPUT);
  analogWrite(DIGITS_VCC, 150);

  updateDisplay(CONF[0], CONF[1], CONF[2], CONF[3]);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  LOG_PRINT("Connecting to WiFi...");
  int animStep = 0;
  unsigned long connectionTimeout = millis() + 20000;

  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() > connectionTimeout)
    {
      LOG_PRINTLN("\nConnection Failed! Rebooting...");
      delay(1000);
      ESP.restart();
    }
    displayAnimationStep(animStep);
    animStep = (animStep + 1) % NUM_ANIM_STEPS;
    delay(50);
    Serial.print(".");
  }

  LOG_PRINTLN("\nWiFi Connected!");
  showLastIPPart();
  OtaConfig();

  telnetServer.begin();
  telnetServer.setNoDelay(true);
  LOG_PRINTLN("\nTelnet server started");

  // *** NEW: Start the UDP listener ***
  udpListener.begin(UDP_PORT);
  LOG_PRINTF("UDP listener started on port %d\n", UDP_PORT);

  timeClient.begin();
  timeClient.update();
  hh = timeClient.getHours();
  mm = timeClient.getMinutes();
  ss = timeClient.getSeconds();
  LOG_PRINTF("Initial time: %02d:%02d:%02d\n", hh, mm, ss);

  setAutoBrightness();
  lastHourChecked = hh;

  displayTime(hh, mm, true);
  targetTime = millis();
}

void loop()
{
  // --- 1. HANDLE TIMEKEEPING ---
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
        {
          hh = 0;
          timeClient.update(); // Daily NTP Sync
          hh = timeClient.getHours();
          mm = timeClient.getMinutes();
          ss = timeClient.getSeconds();
        }
      }
    }
  }

  // --- 2. HANDLE OTA, TELNET, AND UDP ---
  ArduinoOTA.handle();

  // *** NEW: Check for and handle UDP packets ***
  handleUdpCommands();

  if (telnetServer.hasClient())
  {
    if (!telnetClient || !telnetClient.connected())
    {
      if (telnetClient)
        telnetClient.stop();
      telnetClient = telnetServer.accept();
    }
    else
    {
      telnetServer.accept().stop();
    }
  }

  // --- 3. PARSE TELNET COMMANDS ---
  if (telnetClient && telnetClient.connected())
  {
    while (telnetClient.available())
    {
      String input = telnetClient.readStringUntil('\n');
      input.trim();
      if (input.length() > 0)
      {
        LOG_PRINTLN("Received Telnet: " + input);
        char command = input.charAt(0);
        String argument = input.substring(1);
        argument.trim();

        switch (command)
        {
        case 'a': // Auto Brightness
          autoBrightnessEnabled = true;
          lastHourChecked = -1; // Force immediate update
          LOG_PRINTLN("Auto brightness enabled.");
          break;
        case 'b':
        { // Manual Brightness
          autoBrightnessEnabled = false;
          int brightness = constrain(argument.toInt(), 0, 255);
          analogWrite(DIGITS_VCC, brightness);
          LOG_PRINTF("Manual brightness set to %d\n", brightness);
          break;
        }
        case 'f':
        { // Display Float
          displayFloat(argument.toFloat());
          currentMode = OVERRIDE;
          overrideStartTime = millis();
          break;
        }
        case 'i':
        { // Display Integer
          displayInteger(argument.toInt());
          currentMode = OVERRIDE;
          overrideStartTime = millis();
          break;
        }
        case 't':
        { // Display Temperature
          displayTemperature(argument.toFloat());
          currentMode = OVERRIDE;
          overrideStartTime = millis();
          break;
        }
        case 'h':
        { // Set Time
          int sepIndex = argument.indexOf(':');
          if (sepIndex != -1)
          {
            int hours = argument.substring(0, sepIndex).toInt();
            int minutes = argument.substring(sepIndex + 1).toInt();
            if (hours >= 0 && hours < 24 && minutes >= 0 && minutes < 60)
            {
              hh = hours;
              mm = minutes;
              ss = 0;
              lastHourChecked = -1;
              displayTime(hh, mm, true);
            }
          }
          break;
        }
        case 'm': // 'm' for animation
          currentMode = ANIMATION;
          overrideStartTime = millis();
          LOG_PRINTLN("Starting animation via Telnet.");
          break;
        default:
          LOG_PRINTLN("Unknown command");
          break;
        }
      }
    }
  }

  // --- 4. MANAGE DISPLAY STATE ---
  if (autoBrightnessEnabled && hh != lastHourChecked)
  {
    setAutoBrightness();
    lastHourChecked = hh;
  }

  if ((currentMode == OVERRIDE || currentMode == ANIMATION) && (millis() - overrideStartTime > OVERRIDE_DURATION))
  {
    currentMode = IDLE_TIME;
  }

  switch (currentMode)
  {
  case IDLE_TIME:
  {
    bool colonState = (ss % 2 == 0);
    displayTime(hh, mm, colonState);
    break;
  }
  case ANIMATION:
  {
    int animSpeed = 50;
    int animStep = ((millis() - overrideStartTime) / animSpeed) % NUM_ANIM_STEPS;
    displayAnimationStep(animStep);
    break;
  }
  case OVERRIDE:
    // Do nothing, the value is static
    break;
  }
}

// =======================================================
// ==================== FUNCTIONS ========================
// =======================================================

/**
 * @brief *** NEW *** Checks for and processes incoming UDP packets.
 * Command format: a single character followed by a value (e.g., "t23.5", "b100").
 */
void handleUdpCommands()
{
  int packetSize = udpListener.parsePacket();
  if (packetSize)
  {
    int len = udpListener.read(packetBuffer, 255);
    if (len > 0)
    {
      packetBuffer[len] = 0; // Null-terminate the string
    }
    LOG_PRINTF("Received UDP packet from %s: %s\n", udpListener.remoteIP().toString().c_str(), packetBuffer);

    String input = String(packetBuffer);
    input.trim();
    if (input.length() > 0)
    {
      char command = input.charAt(0);
      String argument = input.substring(1);
      argument.trim();

      switch (command)
      {
      case 't':
      { // Display Temperature
        LOG_PRINTF("UDP: Displaying temperature %s\n", argument.c_str());
        displayTemperature(argument.toFloat());
        currentMode = OVERRIDE;
        overrideStartTime = millis();
        break;
      }
      case 'b':
      {                                // Manual Brightness
        autoBrightnessEnabled = false; // A manual setting should disable auto mode
        int brightness = constrain(argument.toInt(), 0, 255);
        analogWrite(DIGITS_VCC, brightness);
        LOG_PRINTF("UDP: Manual brightness set to %d\n", brightness);
        break;
      }
      case 'a': // Auto Brightness
        autoBrightnessEnabled = true;
        lastHourChecked = -1; // Force immediate update
        LOG_PRINTLN("UDP: Auto brightness enabled.");
        break;
      default:
        LOG_PRINTF("UDP: Unknown command '%c'\n", command);
        break;
      }
    }
  }
}

// Function to show the last part of the IP address
void showLastIPPart()
{
  IPAddress ip = WiFi.localIP();
  int lastIPPart = ip[3];
  displayInteger(lastIPPart);
  delay(5000);
}

void displayAnimationStep(int step)
{
  if (step < 0 || step >= NUM_ANIM_STEPS)
    return;
  updateDisplay(animationFrames[step][0], animationFrames[step][1],
                animationFrames[step][2], animationFrames[step][3]);
}

void setAutoBrightness()
{
  int newBrightness;
  if (hh >= 22 || hh < 5)
    newBrightness = BRIGHTNESS_NIGHT;
  else if (hh >= 7 && hh < 19)
    newBrightness = BRIGHTNESS_DAY;
  else
    newBrightness = BRIGHTNESS_DUSK;
  LOG_PRINTF("Auto brightness: Setting level %d for hour %d\n", newBrightness, hh);
  analogWrite(DIGITS_VCC, newBrightness);
}

void displayTime(int hours, int minutes, bool colonOn)
{
  hours = constrain(hours, 0, 23);
  minutes = constrain(minutes, 0, 59);
  int digits[4];
  digits[0] = minutes % 10;
  digits[1] = (minutes / 10) % 10;
  digits[2] = hours % 10;
  digits[3] = (hours / 10) % 10;
  if (digits[3] == 0)
    digits[3] = -2; // Blank leading zero
  byte p1 = getPattern(digits[0], false);
  byte p2 = getPattern(digits[1], false);
  byte p3 = getPattern(digits[2], false);
  byte p4 = getPattern(digits[3], colonOn);
  updateDisplay(p4, p3, p2, p1);
}

void OtaConfig()
{
  ArduinoOTA.setHostname("myesp8266");
  ArduinoOTA.setPassword("admin");
  ArduinoOTA.onStart([]()
                     { LOG_PRINTLN("Start updating " + String((ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem")); });
  ArduinoOTA.onEnd([]()
                   { LOG_PRINTLN("\nEnd"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t)
                        { LOG_PRINTF("Progress: %u%%\r", (p / (t / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
    LOG_PRINTF("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) LOG_PRINTLN("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) LOG_PRINTLN("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) LOG_PRINTLN("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) LOG_PRINTLN("Receive Failed");
    else if (error == OTA_END_ERROR) LOG_PRINTLN("End Failed"); });
  ArduinoOTA.begin();
  LOG_PRINTLN("OTA Ready");
  LOG_PRINT("IP address: ");
  LOG_PRINTLN(WiFi.localIP());
}

byte getPattern(int digit, bool dpOn)
{
  byte pattern = (digit >= 0 && digit <= 9) ? digitPatterns_anode[digit] : (digit == -1 ? MINUS_PATTERN : BLANK_PATTERN);
  if (dpOn)
  {
    pattern |= 0x80;
  }
  return pattern;
}

void updateDisplay(byte pattern4, byte pattern3, byte pattern2, byte pattern1)
{
  digitalWrite(LATCH_PIN, LOW);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern4);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern3);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern2);
  shiftOut(DATA_PIN, CLOCK_PIN, MSBFIRST, pattern1);
  digitalWrite(LATCH_PIN, HIGH);
}

void displayInteger(int num)
{
  num = constrain(num, -999, 9999);
  bool isNegative = num < 0;
  num = abs(num);
  int digits[4] = {num % 10, (num / 10) % 10, (num / 100) % 10, (num / 1000) % 10};
  bool nonZeroFound = false;
  for (int pos = 3; pos > 0; pos--)
  {
    if (digits[pos] == 0 && !nonZeroFound)
    {
      digits[pos] = -2;
    }
    else
    {
      nonZeroFound = true;
    }
  }
  if (isNegative)
  {
    if (digits[3] == -2)
      digits[3] = -1;
    else if (digits[2] == -2)
      digits[2] = -1;
  }
  updateDisplay(getPattern(digits[3], false), getPattern(digits[2], false), getPattern(digits[1], false), getPattern(digits[0], false));
}

void displayFloat(float num)
{
  num = constrain(num, -99.9, 999.9);
  displayTemperature(num); // This function is already designed for this format
}

void displayTemperature(float temp)
{
  if (isnan(temp) || temp > 999.9 || temp < -99.9)
  {
    updateDisplay(MINUS_PATTERN, MINUS_PATTERN, MINUS_PATTERN, MINUS_PATTERN);
    return;
  }
  bool isNegative = (temp < 0);
  float absNum = fabs(temp);
  int value = (int)round(absNum * 10);
  int digits[4] = {value % 10, (value / 10) % 10, (value / 100) % 10, (value / 1000) % 10};

  // Blank leading zeros up to the decimal point
  for (int pos = 3; pos > 1; pos--)
  {
    if (digits[pos] == 0)
    {
      digits[pos] = -2;
    }
    else
    {
      break;
    }
  }

  if (isNegative)
  {
    // Find the first blank spot for the minus sign
    for (int pos = 3; pos >= 0; pos--)
    {
      if (digits[pos] == -2)
      {
        digits[pos] = -1;
        break;
      }
    }
  }
  updateDisplay(getPattern(digits[3], false), getPattern(digits[2], false), getPattern(digits[1], true), getPattern(digits[0], false));
}