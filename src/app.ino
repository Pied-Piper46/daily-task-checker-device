#include <M5Atom.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

// --- WiFi Setting ---
const char* ssid = "elecom-29fffc";
const char* password = "42r797a43a58";

// --- API Setting ---
#define USE_PRODUCTION_API 0 // 0: dev, 1: pro

#if USE_PRODUCTION_API
  const char* API_BASE_URL = "https://daily-task-checker.vercel.app/api";
  // TODO: route authentication
#else
  const char* API_BASE_URL = "http://192.168.1.35:3000/api";
#endif

// --- Device ID Setting ---
const char* DEVICE_ID = "M5AtomMatrixTest";

// --- Display Configuration ---
const int DISPLAY_WIDTH = 5;
const int DISPLAY_HEIGHT = 5;
const int CENTER_X = 2;
const int CENTER_Y = 2;

// --- Animation Configuration ---
const unsigned long SUCCESS_EFFECT_DURATION = 2000;
const unsigned long API_CONNECTING_PULSE_PERIOD = 1000;
const float MAX_RADIUS = 2.5f;

// --- Loop Configuration ---
const unsigned long LOOP_DELAY_MS = 100;

// --- NTP Setting ---
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // UTC
const int daylightOffset_sec = 0; // summer time: 0 in Japan
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, gmtOffset_sec, daylightOffset_sec);

// --- Device Status Definition ---
enum class DeviceState {
  INITIALIZING,        // Initialize WiFi, NTP etc...
  TASK_PENDING,        // Not Done
  API_REQUEST_PENDING, 
  API_CONNECTING,
  EFFECT_SUCCESS,
  TASK_COMPLETED,
  ERROR_RETRYING,
  ERROR_FAILED
};
volatile DeviceState currentState = DeviceState::INITIALIZING;
DeviceState previousState = DeviceState::INITIALIZING;
DeviceState previousDisplayState = DeviceState::INITIALIZING;

// -- LED Color Definition ---
CRGB COLOR_PENDING = CRGB::Red;
CRGB COLOR_API_CONNECTING = CRGB::BlueViolet;
CRGB COLOR_SUCCESS_EFFECT = CRGB::White;
CRGB COLOR_COMPLETED = CRGB::Green;
CRGB COLOR_ERROR = CRGB::Orange;
CRGB COLOR_OFF = CRGB::Black;
CRGB COLOR_INITIALIZING = CRGB::BlueViolet;

unsigned long animationStartTime = 0; // for animation

// --- FreeRTOS ---
TaskHandle_t apiTaskHandle = NULL;
volatile bool apiRequestPending = false;
volatile bool newTargetTaskState = false; // true: DONE, false: NOT_DONE

volatile bool apiResultSuccess = false;
volatile bool apiResultReceived = false;

// --- Midnight Reset Control ---
volatile bool midnightResetExecuted = false;
int lastResetDay = -1;

// --- Prototype Declaration ---
void updateDisplay();
void drawInitializing(unsigned long currentTime);
void drawTaskPending();
void drawApiConnecting(unsigned long currentTime);
void drawEffectSuccess(unsigned long currentTime);
void drawTaskCompleted();
void drawErrorRetrying(unsigned long currentTime);
void drawErrorFailed();
void startSuccessEffect();
void startApiConnectingAnimation();
void apiTaskFunction(void *pvParameters);
bool updateTaskOnApi(const char* deviceId, bool isCompleted);
bool getInitialTaskState(const char* deviceId, bool &isCompletedResult);

// --- Setup ---
void setup() {
  M5.begin(true, false, true);
  Serial.begin(115200);
  Serial.println("M5Atom Matrix Task Checker with FreeRTOS API");
  M5.dis.fillpix(COLOR_INITIALIZING);

  // WiFi Connection
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 20) {
    delay(400);
    Serial.print(".");
    attempt++;
    M5.dis.fillpix((attempt % 2 == 0) ? COLOR_INITIALIZING : COLOR_OFF);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    // M5.dis.fillpix(CRGB::Green);
    delay(200);

    // NTP Client Initialization & Sync time
    timeClient.begin();
    Serial.println("NTP Client started. Syncing time (UTC)...");
    if (timeClient.forceUpdate()) {
      Serial.println("Time synced successfully!");
      Serial.println("Current UTC Time: ");
      Serial.println(timeClient.getFormattedTime());
      // M5.dis.fillpix(CRGB::Blue);
      delay(200);
    } else {
      Serial.println("Time sync failed.");
      M5.dis.fillpix((CRGB::Magenta));
      delay(200);
    }

    // Create API Connection Task
    xTaskCreatePinnedToCore(
      apiTaskFunction, // task function
      "APITask",       // task name
      10000,           // stack size
      NULL,            // task parameter
      1,               // priority
      &apiTaskHandle,  // task handle
      1                // core resource (0 or 1)
    );
    if (apiTaskHandle == NULL) {
      Serial.println("Failed to create API task!");
      currentState = DeviceState::ERROR_FAILED;
    } else {
      Serial.println("API task created!");
    }

    // Initial Device State
    // Get device status from API
    bool initialTaskState = false;
    if (getInitialTaskState(DEVICE_ID, initialTaskState)) {
      Serial.print("Initial task state from API: ");
      Serial.println(initialTaskState ? "DONE" : "NOT_DONE");
      currentState = initialTaskState ? DeviceState::TASK_COMPLETED : DeviceState::TASK_PENDING;
    } else {
      Serial.println("Failed to get initial task state from API. Defaulting to PENDING.");
      currentState = DeviceState::TASK_PENDING;
    }

  } else { // WiFi connection failed
    Serial.println("\nWiFi connection failed");
    currentState = DeviceState::ERROR_FAILED;
  }
  previousState = currentState;
  updateDisplay();
}

// --- LOOP ---
void loop() {
  M5.update();
  unsigned long currentTime = millis();

  // API Task result processing
  if (apiResultReceived) {
    if (currentState == DeviceState::API_CONNECTING) {
      if (apiResultSuccess) {
        Serial.println("API Update successful (from API task)");
        currentState = DeviceState::EFFECT_SUCCESS;
        startSuccessEffect();
      } else {
        Serial.println("API Update failed (from API task)");
        currentState = DeviceState::ERROR_RETRYING;
        animationStartTime = currentTime;
      }
    }
    apiResultReceived = false;
  }

  // Button input processing
  if (M5.Btn.wasPressed() && apiTaskHandle != NULL && !apiRequestPending) {
    Serial.println("Button Pressed!");
    if (currentState == DeviceState::TASK_PENDING) {
      newTargetTaskState = true; // completed
      apiRequestPending = true;
      currentState = DeviceState::API_REQUEST_PENDING;
      Serial.println("Requesting API to set task to COMPLETED.");
    } else if (currentState == DeviceState::TASK_COMPLETED) {
      newTargetTaskState = false; // pending
      apiRequestPending = true;
      currentState = DeviceState::API_REQUEST_PENDING;
      Serial.println("Requesting API to set task to PENDING.");
    } else if (currentState == DeviceState::ERROR_FAILED || currentState == DeviceState::ERROR_RETRYING) {
      Serial.println("Attempting to recover from error state (re-fetch initial)...");
      // WiFi reconnecting, NTP syncing if needed
      // try to get initial state in this time
      bool fetchedState = false;
      if (getInitialTaskState(DEVICE_ID, fetchedState)) {
        currentState = fetchedState ? DeviceState::TASK_COMPLETED : DeviceState::TASK_PENDING;
      } else {
        currentState = DeviceState::ERROR_FAILED;
      }
    }
  }

  // API Request Pending -> API Connecting (for display)
  if (currentState == DeviceState::API_REQUEST_PENDING && apiRequestPending) {
    currentState = DeviceState::API_CONNECTING;
    startApiConnectingAnimation();
  }

  // Change state logic (time base)
  if (currentState == DeviceState::EFFECT_SUCCESS && (currentTime - animationStartTime > 2000)) {
    currentState = newTargetTaskState ? DeviceState::TASK_COMPLETED : DeviceState::TASK_PENDING;
  }
  if (currentState == DeviceState::ERROR_RETRYING && (currentTime - animationStartTime > 5000)) {
    Serial.println("Error retry timeout. Changing to ERROR_FAILED.");
    currentState = DeviceState::ERROR_FAILED;
  }

  // Reset state at 0:01 AM (JST, UTC 15:01)
  if (WiFi.status() == WL_CONNECTED && timeClient.isTimeSet()) {
    int currentDay = timeClient.getDay();
    if (timeClient.getHours() == 15 && timeClient.getMinutes() == 1 && timeClient.getSeconds() < 10) {
      if (!midnightResetExecuted || lastResetDay != currentDay) {
        Serial.println("Midnight reset! Setting task to PENDING.");
        newTargetTaskState = false;
        apiRequestPending = true;
        currentState = DeviceState::API_REQUEST_PENDING;
        midnightResetExecuted = true;
        lastResetDay = currentDay;
      }
    } else {
      midnightResetExecuted = false;
    }
  }

  // Regular display update
  bool needsDisplayUpdate = (currentState != previousDisplayState);

  if (currentState == DeviceState::INITIALIZING ||
      currentState == DeviceState::API_CONNECTING ||
      currentState == DeviceState::EFFECT_SUCCESS ||
      currentState == DeviceState::ERROR_RETRYING) {
        needsDisplayUpdate = true;
  }

  if (needsDisplayUpdate) {
      updateDisplay();
      if (currentState != DeviceState::API_CONNECTING &&
          currentState != DeviceState::EFFECT_SUCCESS &&
          currentState != DeviceState::ERROR_RETRYING &&
          currentState != DeviceState::INITIALIZING) {
            previousDisplayState = currentState;
      }
  }

  delay(LOOP_DELAY_MS);
}


void apiTaskFunction(void *pvParameters) {
  Serial.println("API Task started.");
  for (;;) {
    if (apiRequestPending) {
      Serial.println("API Task: Request received.");
      bool success = updateTaskOnApi(DEVICE_ID, newTargetTaskState);

      apiResultSuccess = success;
      apiResultReceived = true;
      apiRequestPending = false;
      Serial.print("API Task: Request processed. Success: ");
      Serial.println(success);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool updateTaskOnApi(const char* deviceId, bool isCompleted) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("API: WiFi not connected.");
    return false;
  }

  HTTPClient http;
  String apiUrl = String(API_BASE_URL) + "/status";
  Serial.print("API POST URL: ");
  Serial.println(apiUrl);

  http.begin(apiUrl);
  http.addHeader("Content-Type", "application/json");

  StaticJsonDocument<200> jsonDoc;
  jsonDoc["deviceId"] = deviceId;
  jsonDoc["status"] = isCompleted ? "DONE" : "NOT_DONE";
  if (timeClient.isTimeSet()) {
    unsigned long epochTime = timeClient.getEpochTime();

    time_t rawTime = (time_t)epochTime;
    struct tm *timeInfo = gmtime(&rawTime);

    char timestamp[30];
    sprintf(timestamp, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
            timeInfo->tm_year + 1900,
            timeInfo->tm_mon + 1,
            timeInfo->tm_mday,
            timeInfo->tm_hour,
            timeInfo->tm_min,
            timeInfo->tm_sec);
    jsonDoc["timestamp"] = timestamp;
  }

  String requestBody;
  serializeJson(jsonDoc, requestBody);
  Serial.print("API POST Request Body: ");
  Serial.println(requestBody);

  int httpResponseCode = http.POST(requestBody);
  bool success = false;

  if (httpResponseCode > 0) {
    Serial.print("API PUT Response code: ");
    Serial.println(httpResponseCode);
    String responsePayload = http.getString();
    Serial.print("API PUT Response payload: ");
    Serial.println(responsePayload);
    if (httpResponseCode == 200 || httpResponseCode == 201) {
      success = true;
    } else {
      Serial.print("API PUT Error: ");
      Serial.println(http.errorToString(httpResponseCode).c_str());
    }
  } else {
    Serial.print("API PUT Error on send: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  http.end();
  return success;
}


bool getInitialTaskState(const char* deviceId, bool &isCompletedResult) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("API GET: WiFI not connected.");
    return false;
  }

  HTTPClient http;
  String apiUrl = String(API_BASE_URL) + "/devices/" + String(deviceId);
  Serial.print("API GET URL: ");
  Serial.println(apiUrl);

  http.begin(apiUrl);
  int httpResponseCode = http.GET();
  bool success = false;

  if (httpResponseCode == 200) {
    String payload = http.getString();
    Serial.print("API GET Response payload: ");
    Serial.println(payload);
    StaticJsonDocument<512> jsonDoc;
    DeserializationError error = deserializeJson(jsonDoc, payload);
    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
    } else {
      if (jsonDoc.containsKey("status")) {
        String status = jsonDoc["status"].as<String>();
        isCompletedResult = (status == "DONE");
        success = true;
      } else {
        Serial.println("API GET: 'status' field not found in response.");
      }
    }
  } else {
    Serial.print("API GET Error: ");
    Serial.println(httpResponseCode);
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }
  http.end();
  return success;
}


// --- Display Update & Draw Functions
void updateDisplay() {
  unsigned long currentTime = millis();
  M5.dis.clear();

  switch (currentState) {
    case DeviceState::INITIALIZING:
      Serial.println("Called drawInitializing!");
      drawInitializing(currentTime);
      break;
    case DeviceState::TASK_PENDING:
      Serial.println("Called drawTaskPending!");
      drawTaskPending();
      break;
    case DeviceState::API_CONNECTING:
    case DeviceState::API_REQUEST_PENDING:
      Serial.println("Called drawApiConnecting!");
      drawApiConnecting(currentTime);
      break;
    case DeviceState::EFFECT_SUCCESS:
      Serial.println("Called drawEffectSuccess!");
      drawEffectSuccess(currentTime);
      break;
    case DeviceState::TASK_COMPLETED:
      Serial.println("Called drawTaskCompleted!");
      drawTaskCompleted();
      break;
    case DeviceState::ERROR_RETRYING:
      Serial.println("Called drawErrorRetrying!");
      drawErrorRetrying(currentTime);
      break;
    case DeviceState::ERROR_FAILED:
      Serial.println("Called drawErrorFailed!");
      drawErrorFailed();
      break;
  }
}

void drawInitializing(unsigned long currentTime) {
  if ((currentTime / 250) % 2 == 0) {
    M5.dis.fillpix(COLOR_INITIALIZING);
  } else {
    M5.dis.fillpix(COLOR_OFF);
  }
}

void drawTaskPending() {
  M5.dis.fillpix(COLOR_PENDING);
}

void startApiConnectingAnimation() {
  animationStartTime = millis();
}

void drawApiConnecting(unsigned long currentTime) {
  unsigned long elapsed = currentTime - animationStartTime;
  float progress = (float)(elapsed % API_CONNECTING_PULSE_PERIOD) / API_CONNECTING_PULSE_PERIOD;
  if (progress > 1.0) progress = 1.0;

  float currentRadius = MAX_RADIUS * (0.3f + 0.7f * (0.5f + 0.5f * sin(2 * PI * progress)));

  M5.dis.clear();

  for (int y = 0; y < DISPLAY_HEIGHT; y++) {
    for (int x = 0; x < DISPLAY_WIDTH; x++) {
      float dist = sqrt(pow(x - CENTER_X, 2) + pow(y - CENTER_Y, 2));
      if (dist <= currentRadius) {
        float brightness = 1.0f - (dist / currentRadius) * 0.5f;
        CRGB color = COLOR_API_CONNECTING;
        color.r = (uint8_t)(color.r * brightness);
        color.g = (uint8_t)(color.g * brightness);
        color.b = (uint8_t)(color.b * brightness);
        M5.dis.drawpix(x, y, color);
      }
    }
  }
}

void startSuccessEffect() {
  animationStartTime = millis();
}

void drawEffectSuccess(unsigned long currentTime) {
  float progress = (float)(currentTime - animationStartTime) / SUCCESS_EFFECT_DURATION;
  if (progress > 1.0) progress = 1.0;

  M5.dis.clear();

  for (int ring = 0; ring < 3; ring++) {
    float ringDelay = ring * 0.3f;
    float ringProgress = progress - ringDelay;
    if (ringProgress > 0 && ringProgress < 1.0f) {
      float currentRadius = MAX_RADIUS * ringProgress;
      float ringThickness = 0.8f;

      for (int y = 0; y < DISPLAY_HEIGHT; y++) {
        for (int x = 0; x < DISPLAY_WIDTH; x++) {
          float dist = sqrt(pow(x - CENTER_X, 2) + pow(y - CENTER_Y, 2));
          if (dist <= currentRadius && dist >= (currentRadius - ringThickness)) {
            float brightness = 1.0f - ringProgress * 0.3f;
            CRGB color = COLOR_SUCCESS_EFFECT;
            color.r = (uint8_t)(color.r * brightness);
            color.g = (uint8_t)(color.g * brightness);
            color.b = (uint8_t)(color.b * brightness);
            M5.dis.drawpix(x, y, color);
          }
        }
      }
    }
  }
}

void drawTaskCompleted() {
  M5.dis.fillpix(COLOR_COMPLETED);
}

void drawErrorRetrying(unsigned long currentTime) {
  if ((currentTime / 500) % 2 == 0) {
    M5.dis.fillpix(COLOR_ERROR);
  } else {
    M5.dis.fillpix(COLOR_OFF);
  }
}

void drawErrorFailed() {
  M5.dis.fillpix(COLOR_ERROR);
}