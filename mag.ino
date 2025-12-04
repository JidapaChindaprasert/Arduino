// ==============================
// ESP32 Logger with Enhanced NimBLE (based on your code + NimBLE demo)
// ==============================

#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1
#define CONFIG_NIMBLE_TASK_STACK_SIZE 2048
#define CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>

#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLECharacteristic.h>

#include "esp_wifi.h"
#include "esp_bt.h"

// === Config ===
const char gsheetUrl[] PROGMEM = "https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec";
const char deviceName[] = "mag01";
const uint8_t sensorPin = 34;  // ADC1_CH6

// === Preferences namespace ===
const char* NS = "cfg";

// === Globals ===
WebServer* pServerWeb = nullptr;
NimBLECharacteristic* pCharBLE = nullptr;

// Web page (AP config portal)
const char HTTP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset='utf-8'><title>ESP32 Setup</title></head>
<body style='font-family:sans-serif;text-align:center;margin-top:2em'>
<h2>📶 WiFi Configuration</h2>
<form method='post'>
SSID:<br><input name='ssid' required autofocus><br><br>
Password:<br><input name='pass' type='password' required><br><br>
<input type='submit' value='💾 Save & Reboot'>
</form>
</body></html>
)rawliteral";

void handleRoot() {
	if (pServerWeb) pServerWeb->send_P(200, "text/html", HTTP_PAGE);
}

void handleSave() {
	if (!pServerWeb) return;
	String ssid = pServerWeb->arg("ssid");
	String pass = pServerWeb->arg("pass");

	if (ssid.length() && pass.length()) {
		Preferences prefs;
		prefs.begin(NS, false);
		prefs.putString("ssid", ssid);
		prefs.putString("pass", pass);
		prefs.putBool("configured", true);
		prefs.end();

		pServerWeb->send(200, "text/plain", "✅ Saved! Rebooting...");
		delay(1000);
		ESP.restart();
	} else {
		pServerWeb->send(400, "text/plain", "Missing fields!");
	}
}

void startConfigPortal() {
	WiFi.mode(WIFI_AP);
	WiFi.softAP("MAG_Setup_1");

	pServerWeb = new WebServer(80);
	pServerWeb->on("/", HTTP_GET, handleRoot);
	pServerWeb->on("/", HTTP_POST, handleSave);
	pServerWeb->begin();

	Serial.println(F("⚙️ Open http://192.168.4.1"));
}

// ===== Enhanced BLE Callbacks (from NimBLE demo, simplified) =====
class MyServerCallbacks : public NimBLEServerCallbacks {
	void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
		Serial.printf("🔵 BLE client connected: %s\n", connInfo.getAddress().toString().c_str());
		// Optional: request faster connection params
		// pServer->updateConnParams(connInfo.getConnHandle(), 12, 24, 0, 60);
	}

	void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
		Serial.printf("🔴 BLE client disconnected (reason: %d), restarting advertising\n", reason);
		NimBLEDevice::startAdvertising();
	}

	void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
		Serial.printf("📈 MTU updated to %u\n", MTU);
	}
};

void setupBLE() {
	NimBLEDevice::init(deviceName);
	// Disable bonding/encryption (matches your original secure-less setup)
	NimBLEDevice::setSecurityAuth(false, false, false);

	NimBLEServer* pServer = NimBLEDevice::createServer();
	pServer->setCallbacks(new MyServerCallbacks());

	NimBLEService* pService = pServer->createService("181A");  // Environmental Sensing
	pCharBLE = pService->createCharacteristic(
	  "2A58",  // Analog
	  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
	pCharBLE->setValue("0");

	pService->start();

	NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
	pAdv->setName(deviceName);
	pAdv->addServiceUUID("181A");
	pAdv->start();

	Serial.println(F("🔵 BLE advertising started (notify on 2A58)"));
}

// ==============================
// Google Sheets
// ==============================
bool sendToGoogleSheet(int raw, float voltage) {
	if (WiFi.status() != WL_CONNECTED) return false;

	char voltStr[10];
	dtostrf(voltage, 1, 3, voltStr);

	char url[250];
	// Remove PROGMEM for snprintf compatibility (or use strcpy_P if needed)
	strcpy(url, "https://script.google.com/macros/s/");
	strcat(url, "AKfycbyc1rcp6tBMwlt_A2NjyrVlQ1wyJlIvG5Qncs4gKgFcVNrni7LZd-mus3ayOe_OfJAv");  // ⚠️ Replace with actual ID
	strcat(url, "/exec?device=");
	strcat(url, deviceName);
	char temp[20];
	sprintf(temp, "&raw=%d&voltage=%s", raw, voltStr);
	strcat(url, temp);

	HTTPClient http;
	http.begin(url);
	http.setTimeout(8000);
	http.setConnectTimeout(5000);
	http.setUserAgent("ESP32-BLE");

	int code = http.GET();
	http.end();

	Serial.printf("📡 HTTP %d\n", code);
	return (code == 200 || code == 302);
}

// ==============================
// Setup & Loop
// ==============================
bool isConfigMode = false;

void setup() {
	Serial.begin(115200);
	esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
	esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

	delay(300);
	Serial.println(F("\n=== ESP32 Logger (BLE + Google Sheets) ==="));

	Preferences prefs;
	prefs.begin(NS, true);
	bool configured = prefs.getBool("configured", false);
	prefs.end();

	if (configured) {
		prefs.begin(NS, true);
		String ssid = prefs.getString("ssid");
		String pass = prefs.getString("pass");
		prefs.end();

		if (ssid.length() && pass.length()) {
			Serial.print(F("📶 Connecting to: "));
			Serial.println(ssid);
			WiFi.mode(WIFI_STA);
			WiFi.begin(ssid.c_str(), pass.c_str());

			unsigned long start = millis();
			while (WiFi.status() != WL_CONNECTED && (millis() - start) < 12000) {
				delay(300);
				Serial.print(F("."));
			}

			if (WiFi.status() == WL_CONNECTED) {
				Serial.println(F("\n✅ WiFi connected"));
				Serial.print(F("IP: "));
				Serial.println(WiFi.localIP());
				isConfigMode = false;
			} else {
				configured = false;
			}
			setupBLE();  // Start BLE only when WiFi is ready
		}
	}

	if (!configured) {
		isConfigMode = true;
		startConfigPortal();
	}
}

void loop() {
	if (isConfigMode) {
		if (pServerWeb) pServerWeb->handleClient();
		delay(10);
	}

	// Read sensor
	int raw = analogRead(sensorPin);
	float voltage = (raw / 4095.0f) * 3.3f;

	// BLE notify every 300ms
	static unsigned long lastBLE = 0;
	if (millis() - lastBLE > 300 && pCharBLE) {
		char buf[8];
		snprintf(buf, sizeof(buf), "%d", raw);
		pCharBLE->setValue((uint8_t*)buf, strlen(buf));
		pCharBLE->notify();
		lastBLE = millis();
	}

	// Upload to Google Sheets every 5s
	static unsigned long lastSheet = 0;
	if (WiFi.status() == WL_CONNECTED && millis() - lastSheet > 5000) {
		bool sent = sendToGoogleSheet(raw, voltage);
		Serial.println(sent ? F("✅ Sheet updated") : F("❌ Sheet failed"));
		lastSheet = millis();
	}

	// Debug print every 1s
	static unsigned long lastDebug = 0;
	if (millis() - lastDebug > 1000) {
		Serial.printf("Raw: %d | V: %.3f V\n", raw, voltage);
		lastDebug = millis();
	}

	delay(20);
}