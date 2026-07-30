#include <Wire.h>
#include <MPU6050.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <TinyGPS.h>

#define GPS_RX      4
#define GPS_TX      2
#define GSM_RX      16
#define GSM_TX      17
#define BUZZER_PIN  25
#define LED_PIN     26
#define BUTTON_PIN  27

HardwareSerial GPS(1);
HardwareSerial GSM(2);
MPU6050 mpu;


#define NUM_CONTACTS 2
const String emergencyContacts[NUM_CONTACTS] = {
  "+256754865111",
  "+256702124974"
};

TinyGPS tgps;
boolean newData = false;
float latitude = 0, longitude = 0;

const char* WIFI_SSID     = "Ziporah";
const char* WIFI_PASSWORD = "ziporah12";
const String API_KEY      = "AIzaSyB3MUXR_0nG5TYcercM46MnulfP1pJhGsQ";


const float CRASH_THRESHOLD_G = 2.7f;
const unsigned long CONFIRM_WINDOW_MS = 10000UL;
const unsigned long CRASH_COOLDOWN_MS = 30000UL;
const unsigned long GPS_TIMEOUT_MS = 8000UL;
const unsigned long CALL_RING_MS = 10000UL;


bool alertActive = false;
unsigned long alertStartTime = 0;
unsigned long lastCrashTime = 0;

void cancelAlert();
void startAlert();
void processConfirmedAlert();

bool readGPS(float &lat, float &lng, unsigned long timeoutMs = GPS_TIMEOUT_MS);
bool parseNMEA(const String &nmea, float &lat, float &lng);
bool parseGPRMC(String nmea, float &lat, float &lng);
bool parseGPGGA(String nmea, float &lat, float &lng);
float convertToDecimalDegrees(const String &rawCoord, const String &direction);

bool connectWiFi(unsigned long timeoutMs = 15000UL);
bool findNearestPoliceStation(float lat, float lng, String &phoneNumber, String &centerName);
bool findNearestHospital(float lat, float lng, String &phoneNumber, String &centerName);
bool getPlacePhoneNumber(const String &placeId, String &phoneNumber);
String httpGET(const String &url);

String sendATCommand(const String &cmd, unsigned long timeout);
bool initGSM();
bool sendSMS(const String &number, const String &message, uint8_t retries = 2);
void makeAlertCall(const String &number, unsigned long ringDurationMs = CALL_RING_MS);
void clearGSMBuffer();
String normalizePhoneNumber(String number);

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin();
  mpu.initialize();

  GPS.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  GSM.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  Serial.println("\n=== Smart Crash Alert System Boot ===");
  Serial.println(mpu.testConnection() ? "MPU6050 connected!" : "MPU6050 connection failed!");

  if (!initGSM()) {
    Serial.println("Warning: GSM init failed. SMS may not work.");
  }

  connectWiFi(8000UL);
}

void loop() {
  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);

  float axg = ax / 16384.0f;
  float ayg = ay / 16384.0f;
  float azg = az / 16384.0f;

  float amag = sqrtf(axg * axg + ayg * ayg + azg * azg);

  unsigned long now = millis();
  bool cooldownPassed = (now - lastCrashTime) > CRASH_COOLDOWN_MS;

  if (!alertActive && cooldownPassed && amag >= CRASH_THRESHOLD_G) {
    Serial.printf("Crash-like event detected! |A|=%.2f g\n", amag);
    startAlert();
    lastCrashTime = now;
  }

  if (alertActive) {
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("Alert cancelled by user.");
      cancelAlert();
      delay(250);
      return;
    }

    if (now - alertStartTime >= CONFIRM_WINDOW_MS) {
      Serial.println("Alert confirmed. Dispatching emergency messages...");
      processConfirmedAlert();
      cancelAlert();
    } else {
      digitalWrite(LED_PIN, ((now / 250) % 2) ? HIGH : LOW);
    }
  } else {
    digitalWrite(LED_PIN, LOW);
  }

  delay(30);
}

void startAlert() {
  alertActive = true;
  alertStartTime = millis();
  digitalWrite(BUZZER_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
  Serial.println("Press cancel button within 10 seconds to stop alert.");
}

void cancelAlert() {
  alertActive = false;
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

void processConfirmedAlert() {
  float lat = 0.0f, lng = 0.0f;
  bool gpsOk = readGPS(lat, lng);

  String locationUrl = gpsOk
    ? "https://maps.google.com/?q=" + String(lat, 6) + "," + String(lng, 6)
    : "GPS unavailable";

  String msgFamily = "Emergency! Crash detected. Location: " + locationUrl;

  for (int i = 0; i < NUM_CONTACTS; i++) {
    String number = normalizePhoneNumber(emergencyContacts[i]);
    makeAlertCall(number, CALL_RING_MS);
    bool ok = sendSMS(number, msgFamily, 2);
    Serial.println(ok ? "Contact SMS sent." : "Contact SMS failed.");
    delay(500);
  }

  if (!gpsOk) {
    Serial.println("Skipping dynamic emergency center notification: GPS unavailable.");
    return;
  }

  if (!connectWiFi()) {
    Serial.println("Skipping police/hospital lookup: WiFi unavailable.");
    return;
  }

  String phoneNumber, centerName;
  bool found = findNearestPoliceStation(lat, lng, phoneNumber, centerName);

  if (!found) {
    Serial.println("No valid police station found, trying nearest hospital...");
    found = findNearestHospital(lat, lng, phoneNumber, centerName);
  }

  if (!found) {
    Serial.println("No nearby police or hospital with a valid phone number found.");
    return;
  }

  // Google may return the number in local format (e.g. 0800199990) or
  // international format - normalizePhoneNumber() converts either into +256...
  phoneNumber = normalizePhoneNumber(phoneNumber);

  // Display-only: we found the nearest center and its number, but we do NOT
  // call or text it automatically. Calling/texting stays limited to the
  // fixed emergencyContacts[] list above.
  Serial.println("Nearest emergency center found: " + centerName);
  Serial.println("Phone number: " + phoneNumber);
  Serial.println("(Auto-call/SMS to this number is disabled - display only.)");
}


bool readGPS(float &lat, float &lng, unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (GPS.available()) {
      String nmea = GPS.readStringUntil('\n');
      nmea.trim();
      if (nmea.length() < 6) continue;

      if (nmea.startsWith("$GPRMC") || nmea.startsWith("$GNRMC") ||
          nmea.startsWith("$GPGGA") || nmea.startsWith("$GNGGA")) {
        if (parseNMEA(nmea, lat, lng)) {
          Serial.printf("GPS fix: %.6f, %.6f\n", lat, lng);
          return true;
        }
      }
    }
    delay(5);
  }
  return false;
}

bool parseNMEA(const String &nmea, float &lat, float &lng) {
  if (nmea.startsWith("$GPRMC") || nmea.startsWith("$GNRMC")) {
    return parseGPRMC(nmea, lat, lng);
  }
  if (nmea.startsWith("$GPGGA") || nmea.startsWith("$GNGGA")) {
    return parseGPGGA(nmea, lat, lng);
  }
  return false;
}

bool parseGPRMC(String nmea, float &lat, float &lng) {
  String parts[20];
  int idx = 0;

  while (nmea.length() && idx < 20) {
    int comma = nmea.indexOf(',');
    if (comma < 0) {
      parts[idx++] = nmea;
      break;
    }
    parts[idx++] = nmea.substring(0, comma);
    nmea = nmea.substring(comma + 1);
  }

  if (idx < 7) return false;
  if (parts[2] != "A") return false;
  if (parts[3].length() == 0 || parts[5].length() == 0) return false;

  lat = convertToDecimalDegrees(parts[3], parts[4]);
  lng = convertToDecimalDegrees(parts[5], parts[6]);
  return !(lat == 0.0f && lng == 0.0f);
}

bool parseGPGGA(String nmea, float &lat, float &lng) {
  String parts[20];
  int idx = 0;

  while (nmea.length() && idx < 20) {
    int comma = nmea.indexOf(',');
    if (comma < 0) {
      parts[idx++] = nmea;
      break;
    }
    parts[idx++] = nmea.substring(0, comma);
    nmea = nmea.substring(comma + 1);
  }

  if (idx < 7) return false;
  if (parts[6].toInt() == 0) return false;
  if (parts[2].length() == 0 || parts[4].length() == 0) return false;

  lat = convertToDecimalDegrees(parts[2], parts[3]);
  lng = convertToDecimalDegrees(parts[4], parts[5]);
  return !(lat == 0.0f && lng == 0.0f);
}

float convertToDecimalDegrees(const String &rawCoord, const String &direction) {
  if (rawCoord.length() < 4) return 0.0f;

  float degrees = 0.0f;
  float minutes = 0.0f;

  if (direction == "N" || direction == "S") {
    if (rawCoord.length() < 4) return 0.0f;
    degrees = rawCoord.substring(0, 2).toFloat();
    minutes = rawCoord.substring(2).toFloat();
  } else if (direction == "E" || direction == "W") {
    if (rawCoord.length() < 5) return 0.0f;
    degrees = rawCoord.substring(0, 3).toFloat();
    minutes = rawCoord.substring(3).toFloat();
  } else {
    return 0.0f;
  }

  float dd = degrees + (minutes / 60.0f);
  if (direction == "S" || direction == "W") dd *= -1.0f;
  return dd;
}


bool connectWiFi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start < timeoutMs)) {
    Serial.print(".");
    delay(250);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    return true;
  }

  Serial.println("WiFi connect timeout.");
  return false;
}

String httpGET(const String &url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP GET skipped: WiFi not connected.");
    return "";
  }

  HTTPClient http;
  http.begin(url);
  http.setTimeout(8000);

  int code = http.GET();
  String payload;

  if (code > 0) {
    if (code == HTTP_CODE_OK) {
      payload = http.getString();
    } else {
      Serial.println("HTTP code: " + String(code));
      payload = http.getString();
    }
  } else {
    Serial.println("HTTP request failed: " + http.errorToString(code));
  }

  http.end();
  return payload;
}

// Loops through nearby POLICE results, validates each, and picks the first
// one that passes: has "police" type, is operational, has ratings data, AND has a phone number
bool findNearestPoliceStation(float lat, float lng, String &phoneNumber, String &centerName) {
  String url = "https://maps.googleapis.com/maps/api/place/nearbysearch/json?location=" +
               String(lat, 6) + "," + String(lng, 6) +
               "&rankby=distance&type=police&key=" + API_KEY;

  String response = httpGET(url);
  if (response.length() == 0) return false;

  DynamicJsonDocument doc(8192);
  auto err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Places Nearby (police) JSON parse error.");
    return false;
  }

  const char* status = doc["status"] | "";
  if (String(status) != "OK") {
    Serial.println("Places Nearby (police) status: " + String(status));
    return false;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return false;

  for (JsonObject place : results) {
    String placeId = place["place_id"].as<String>();
    String name = place["name"].as<String>();

    bool hasPoliceType = false;
    JsonArray types = place["types"].as<JsonArray>();
    for (JsonVariant t : types) {
      if (String(t.as<const char*>()) == "police") {
        hasPoliceType = true;
        break;
      }
    }

    String businessStatus = place["business_status"] | "";
    bool isOperational = (businessStatus == "OPERATIONAL");
    bool hasRatingData = place.containsKey("user_ratings_total");

    if (!hasPoliceType || !isOperational || !hasRatingData) {
      Serial.println("Skipping police candidate (failed validation): " + name +
                      " [police_type=" + String(hasPoliceType) +
                      ", operational=" + String(isOperational) +
                      ", has_ratings=" + String(hasRatingData) + "]");
      continue;
    }

    String phone;
    if (getPlacePhoneNumber(placeId, phone)) {
      phoneNumber = phone;
      centerName = name;
      Serial.println("Selected police station: " + name);
      return true;
    } else {
      Serial.println("Skipping police (no phone number): " + name);
    }
  }

  return false;
}


bool findNearestHospital(float lat, float lng, String &phoneNumber, String &centerName) {
  String url = "https://maps.googleapis.com/maps/api/place/nearbysearch/json?location=" +
               String(lat, 6) + "," + String(lng, 6) +
               "&rankby=distance&type=hospital&key=" + API_KEY;

  String response = httpGET(url);
  if (response.length() == 0) return false;

  DynamicJsonDocument doc(8192);
  auto err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Places Nearby (hospital) JSON parse error.");
    return false;
  }

  const char* status = doc["status"] | "";
  if (String(status) != "OK") {
    Serial.println("Places Nearby (hospital) status: " + String(status));
    return false;
  }

  JsonArray results = doc["results"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) return false;

  for (JsonObject place : results) {
    String placeId = place["place_id"].as<String>();
    String name = place["name"].as<String>();

    bool hasHospitalType = false;
    JsonArray types = place["types"].as<JsonArray>();
    for (JsonVariant t : types) {
      if (String(t.as<const char*>()) == "hospital") {
        hasHospitalType = true;
        break;
      }
    }

    String businessStatus = place["business_status"] | "";
    bool isOperational = (businessStatus == "OPERATIONAL");
    bool hasRatingData = place.containsKey("user_ratings_total");

    if (!hasHospitalType || !isOperational || !hasRatingData) {
      Serial.println("Skipping hospital candidate (failed validation): " + name);
      continue;
    }

    String phone;
    if (getPlacePhoneNumber(placeId, phone)) {
      phoneNumber = phone;
      centerName = name;
      Serial.println("Selected hospital: " + name);
      return true;
    } else {
      Serial.println("Skipping hospital (no phone number): " + name);
    }
  }

  return false;
}

bool getPlacePhoneNumber(const String &placeId, String &phoneNumber) {
  String url = "https://maps.googleapis.com/maps/api/place/details/json?place_id=" +
               placeId + "&fields=formatted_phone_number,types,business_status,user_ratings_total&key=" + API_KEY;

  String response = httpGET(url);
  if (response.length() == 0) return false;

  DynamicJsonDocument doc(4096);
  auto err = deserializeJson(doc, response);
  if (err) {
    Serial.println("Place details JSON parse error.");
    return false;
  }

  const char* status = doc["status"] | "";
  if (String(status) != "OK") {
    Serial.println("Place details status: " + String(status));
    return false;
  }

  if (!doc["result"]["formatted_phone_number"].is<const char*>()) return false;

  phoneNumber = doc["result"]["formatted_phone_number"].as<String>();
  phoneNumber = normalizePhoneNumber(phoneNumber);
  return phoneNumber.length() > 0;
}


bool initGSM() {
  Serial.println("Initializing GSM...");
  String r1 = sendATCommand("AT", 1000);
  String r2 = sendATCommand("ATE0", 1000);
  String r3 = sendATCommand("AT+CMGF=1", 1000);
  String r4 = sendATCommand("AT+CSQ", 1000);
  String r5 = sendATCommand("AT+CREG?", 1000);

  bool ok = (r1.indexOf("OK") != -1) && (r3.indexOf("OK") != -1);
  Serial.println(ok ? "GSM ready." : "GSM not fully ready.");
  (void)r2; (void)r4; (void)r5;
  return ok;
}

String sendATCommand(const String &cmd, unsigned long timeout) {
  clearGSMBuffer();
  GSM.println(cmd);

  String response;
  unsigned long start = millis();
  while (millis() - start < timeout) {
    while (GSM.available()) {
      response += (char)GSM.read();
    }
    if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) break;
  }

  Serial.println(">> " + cmd);
  Serial.println("<< " + response);
  return response;
}

void makeAlertCall(const String &number, unsigned long ringDurationMs) {
  String clean = normalizePhoneNumber(number);
  Serial.println("Calling " + clean + " for " + String(ringDurationMs / 1000) + " seconds...");

  clearGSMBuffer();
  GSM.print("ATD");
  GSM.print(clean);
  GSM.println(";");

  unsigned long start = millis();
  String resp;
  while (millis() - start < ringDurationMs) {
    while (GSM.available()) {
      resp += (char)GSM.read();
    }
  }

  Serial.println("Call response: " + resp);

  clearGSMBuffer();
  GSM.println("ATH"); // hang up
  delay(1000);
  while (GSM.available()) GSM.read();

  Serial.println("Call ended.");
}

bool sendSMS(const String &number, const String &message, uint8_t retries) {
  String clean = normalizePhoneNumber(number);
  if (clean.length() < 8) {
    Serial.println("Invalid number: " + clean);
    return false;
  }

  for (uint8_t attempt = 0; attempt <= retries; attempt++) {
    Serial.printf("Sending SMS to %s (attempt %u)\n", clean.c_str(), attempt + 1);

    sendATCommand("AT+CMGF=1", 1000); // re-assert text mode before every attempt

    clearGSMBuffer();
    GSM.print("AT+CMGS=\"");
    GSM.print(clean);
    GSM.println("\"");

    unsigned long promptStart = millis();
    String promptResp;
    bool gotPrompt = false;
    while (millis() - promptStart < 3000) {
      while (GSM.available()) {
        char c = (char)GSM.read();
        promptResp += c;
        if (c == '>') {
          gotPrompt = true;
          break;
        }
      }
      if (gotPrompt) break;
    }

    if (!gotPrompt) {
      Serial.println("No CMGS prompt. Resp: " + promptResp);
      delay(500);
      continue;
    }

    GSM.print(message);
    GSM.write(26);

    String resp;
    unsigned long start = millis();
    while (millis() - start < 20000) {
      while (GSM.available()) resp += (char)GSM.read();
      if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1 || resp.indexOf("+CMS ERROR") != -1) {
        break;
      }
    }

    Serial.println("SMS modem response: " + resp);

    if (resp.indexOf("OK") != -1) {
      Serial.println("SMS sent successfully.");
      return true;
    }
    delay(700);
  }

  Serial.println("SMS failed after retries.");
  return false;
}

void clearGSMBuffer() {
  while (GSM.available()) GSM.read();
}

String normalizePhoneNumber(String number) {
  number.trim();
  number.replace(" ", "");
  number.replace("-", "");
  number.replace("(", "");
  number.replace(")", "");

  if (number.startsWith("00")) {
    number = "+" + number.substring(2);
  } else if (number.startsWith("0")) {
    number = "+256" + number.substring(1);
  }

  return number;
}
