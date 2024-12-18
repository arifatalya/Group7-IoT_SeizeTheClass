#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <painlessMesh.h>
#include <WiFi.h>
#include <time.h>
#include <DHT.h>

#define DHTTYPE DHT11
#define DHTPIN 15
#define MOTION_PIN 16
#define RELAY_LAMP 17
#define RELAY_AC 18

#define MESH_SSID "SeizeTheClass"
#define MESH_PASSWORD "runincircles"
#define MESH_PORT 5007

#define MOTION_THRESHOLD 5  
#define TIMER_DURATION 120 * 60 * 1000  


DHT dht(DHTPIN, DHTTYPE);
painlessMesh mesh;

bool autoMode = true;
unsigned long motionCounter = 0;
unsigned long relayOffTime = 0;
bool relaysActive = false;
String classStartTime = "08:00";
String classEndTime = "10:00";

SemaphoreHandle_t xMutex;

TaskHandle_t controlTaskHandle = NULL;

void taskControlAuto(void *pvParameters);
void taskControlOverride(void *pvParameters);
void taskMotion(void *pvParameters);
void taskDHT(void *pvParameters);
void taskMesh(void *pvParameters);
void sendRelayStatus();
void handleReceivedMessage(String &msg);
void setupTimeSync();
bool isClassActive();

void setup() {
    Serial.begin(115200);

    dht.begin();
    pinMode(MOTION_PIN, INPUT);
    pinMode(RELAY_LAMP, OUTPUT);
    pinMode(RELAY_AC, OUTPUT);
    
    digitalWrite(RELAY_LAMP, LOW);
    digitalWrite(RELAY_AC, LOW);

    mesh.setDebugMsgTypes(ERROR | STARTUP | CONNECTION);
    mesh.init(MESH_SSID, MESH_PASSWORD, MESH_PORT);
    mesh.onReceive([](uint32_t from, String &msg) {
        handleReceivedMessage(msg);
    });

    xMutex = xSemaphoreCreateMutex();
    if (xMutex == NULL) {
        Serial.println("Failed to create mutex. Halting.");
        while (1);
    }

    setupTimeSync();

    if (xTaskCreate(taskControlAuto, "Task_Control_Auto", 2048, NULL, 1, &controlTaskHandle) != pdPASS) {
        Serial.println("Failed to create Task_Control_Auto");
    }
    if (xTaskCreate(taskMotion, "Task_Motion", 1024, NULL, 1, NULL) != pdPASS) {
        Serial.println("Failed to create Task_Motion");
    }
    if (xTaskCreate(taskDHT, "Task_DHT", 2048, NULL, 1, NULL) != pdPASS) {
        Serial.println("Failed to create Task_DHT");
    }
    if (xTaskCreate(taskMesh, "Task_Mesh", 4096, NULL, 1, NULL) != pdPASS) {
        Serial.println("Failed to create Task_Mesh");
    }
}

void loop() {
    mesh.update();
}

void setupTimeSync() {
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

bool isClassActive() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char currentTime[6];
    strftime(currentTime, sizeof(currentTime), "%H:%M", &timeinfo);
    return (classStartTime <= String(currentTime) && classEndTime >= String(currentTime));
}

void taskControlAuto(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
            if (motionCounter >= MOTION_THRESHOLD) {
                if (!relaysActive) {
                    digitalWrite(RELAY_LAMP, HIGH);
                    digitalWrite(RELAY_AC, HIGH);
                    relaysActive = true;
                    relayOffTime = millis() + TIMER_DURATION;
                    Serial.println("Relays ON (Auto Mode)");
                }
            }

            if (relaysActive && millis() > relayOffTime) {
                digitalWrite(RELAY_LAMP, LOW);
                digitalWrite(RELAY_AC, LOW);
                relaysActive = false;
                motionCounter = 0;
                Serial.println("Relays OFF (Timer expired)");
            }

            xSemaphoreGive(xMutex);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void taskControlOverride(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
            if (isClassActive()) {
                digitalWrite(RELAY_LAMP, HIGH);
                digitalWrite(RELAY_AC, HIGH);
                Serial.println("Override Mode: Relays ON (Class Active)");
            } else {
                digitalWrite(RELAY_LAMP, LOW);
                digitalWrite(RELAY_AC, LOW);
                Serial.println("Override Mode: Relays OFF (Class Inactive)");
            }
            xSemaphoreGive(xMutex);
        }
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void taskMotion(void *pvParameters) {
    while (1) {
        if (digitalRead(MOTION_PIN) == HIGH) {
            motionCounter++;
            Serial.println("Motion detected. Counter: " + String(motionCounter));
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

void taskDHT(void *pvParameters) {
    while (1) {
        double temperature = dht.readTemperature();
        double humidity = dht.readHumidity();
        if (!isnan(temperature) && !isnan(humidity)) {
            Serial.println("Temperature: " + String(temperature) + "°C, Humidity: " + String(humidity) + "%");
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void taskMesh(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(xMutex, portMAX_DELAY) == pdTRUE) {
            String status = "RELAY_STATUS:" + String(digitalRead(RELAY_LAMP) ? "ON" : "OFF");
            mesh.sendBroadcast(status);
            xSemaphoreGive(xMutex);
        }
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

void handleReceivedMessage(String &msg) {
    if (msg.startsWith("OVERRIDE_ON")) {
        autoMode = false;
        vTaskDelete(controlTaskHandle);
        xTaskCreate(taskControlOverride, "Task_Control_Override", 2048, NULL, 1, &controlTaskHandle);
        Serial.println("Switched to Override Mode");
    } else if (msg.startsWith("OVERRIDE_OFF")) {
        autoMode = true;
        vTaskDelete(controlTaskHandle);
        xTaskCreate(taskControlAuto, "Task_Control_Auto", 2048, NULL, 1, &controlTaskHandle);
        Serial.println("Switched to Auto Mode");
    }
}