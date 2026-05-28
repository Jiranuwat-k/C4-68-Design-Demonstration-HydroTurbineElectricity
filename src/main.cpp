/*
  Arduino IoT Cloud Multi-Sensor Monitor
  Refactored version with improved code organization
*/

#include "thingProperties.h"
#include <ModbusMasterPzem017.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>

// ============================================================================
// CONFIGURATION SECTION
// ============================================================================

// Pin Definitions
#define FLOW_SENSOR_PIN     27
#define PRESSURE_ADC_PIN    34
#define HALL_SENSOR_PIN     26
#define LED_POWER 19
#define LED_STATUS 18

// PZEM-017 Configuration
#define PZEM_SLAVE_ADDR     0x01
#define PZEM_SHUNT_ADDR     0x0001  // 0x0000=100A, 0x0001=50A, 0x0002=200A, 0x0003=300A

// Sensor Calibration
#define FLOW_CALIBRATION    4.5
#define VOLTAGE_DIVIDER     0.6875  // 10k:22k divider
#define RPM_PULSES_PER_REV  4       // จำนวนแม่เหล็ก (4 ขั้ว)

// Task Update Intervals (milliseconds)
#define CLOUD_UPDATE_RATE   2000
#define PZEM_READ_RATE      5000
#define LCD_UPDATE_RATE     1000
#define FLOW_READ_RATE      100
#define PRESSURE_READ_RATE  500
#define RPM_READ_RATE       100

// PZEM stale-data protection
#define PZEM_MAX_FAIL       3   // รีเซ็ต V/I/P หลังอ่านล้มเหลวติดต่อกัน N ครั้ง

// RPM Moving Average
#define RPM_AVG_SIZE        5   // จำนวนตัวอย่างสำหรับหาค่าเฉลี่ย RPM

// ============================================================================
// GLOBAL OBJECTS & VARIABLES
// ============================================================================

// Hardware Objects
ModbusMaster pzemNode;
LiquidCrystal_I2C lcd(0x27, 20, 4);

// Sensor Data Structures
struct PowerData {
  float voltage = 0.0;
  float current = 0.0;
  float power = 0.0;
  float energy = 0.0;
  bool  valid   = false; // false = อ่านไม่ได้ (แรงดันต่ำเกินไป)
} powerData;

struct FlowData {
  float rate = 0.0;           // L/min
  unsigned int milliLitres = 0;
  unsigned long totalML = 0;
  volatile byte pulseCount = 0;
} flowData;

struct PressureData {
  float bar = 0.0;
  float mpa = 0.0;
} pressureData;

struct RPMData {
  float rpmCount = 0.0;              // วิธี Pulse Count
  float rpmAvg   = 0.0;              // ค่าเฉลี่ย Moving Average
  volatile unsigned long pulseCount = 0;
} rpmData;

// RPM Moving Average buffer
float rpmBuffer[RPM_AVG_SIZE] = {0};
int   rpmBufIndex = 0;

// FreeRTOS Resources
TaskHandle_t hTaskCloud, hTaskPZEM, hTaskLCD, hTaskFlow, hTaskPressure, hTaskRPM;
SemaphoreHandle_t xMutex;

// ============================================================================
// INTERRUPT SERVICE ROUTINES
// ============================================================================

void IRAM_ATTR flowPulseISR() {
  flowData.pulseCount++;
}

void IRAM_ATTR rpmPulseISR() {
  static volatile unsigned long lastPulseTime = 0;
  unsigned long now = micros();
  if (now - lastPulseTime > 2000) { // 2ms software debounce
    rpmData.pulseCount++;
    lastPulseTime = now;
  }
}

// ============================================================================
// SENSOR READING FUNCTIONS
// ============================================================================

// ตัวนับการอ่านล้มเหลวติดต่อกันของ PZEM
static uint8_t pzemFailCount = 0;

bool readPowerSensor() {
  uint8_t result = pzemNode.readInputRegisters(0x0000, 6);

  if (result != pzemNode.ku8MBSuccess) {
    Serial.println("PZEM read failed");
    pzemFailCount++;
    if (pzemFailCount >= PZEM_MAX_FAIL) {
      // แรงดันต่ำกว่า 7V → PZEM อ่านไม่ได้ → mark ว่าไม่ valid
      if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(100))) {
        powerData.voltage = 0.0;
        powerData.current = 0.0;
        powerData.power   = 0.0;
        powerData.valid   = false;
        xSemaphoreGive(xMutex);
      }
      pzemFailCount = 0;
    }
    return false;
  }

  pzemFailCount = 0; // อ่านสำเร็จ – เคลียร์ fail counter

  if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(100))) {
    // Voltage (register 0)
    powerData.voltage = pzemNode.getResponseBuffer(0x0000) / 100.0;

    // Current (register 1)
    powerData.current = pzemNode.getResponseBuffer(0x0001) / 100.0;

    // Power (registers 2-3, 32-bit)
    uint32_t powerRaw = ((uint32_t)pzemNode.getResponseBuffer(0x0003) << 16) |
                        pzemNode.getResponseBuffer(0x0002);
    powerData.power = powerRaw / 10.0;

    // Energy (registers 4-5, 32-bit)
    uint32_t energyRaw = ((uint32_t)pzemNode.getResponseBuffer(0x0005) << 16) |
                         pzemNode.getResponseBuffer(0x0004);
    powerData.energy = energyRaw;

    powerData.valid = true; // อ่านสำเร็จ – ข้อมูลเชื่อถือได้
    xSemaphoreGive(xMutex);
    return true;
  }
  return false;
}

void readFlowSensor(unsigned long deltaTime) {
  static portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&flowMux);
  byte pulses = flowData.pulseCount;
  flowData.pulseCount = 0;
  portEXIT_CRITICAL(&flowMux);

  // Calculate flow rate: (pulses/sec) / calibrationFactor = L/min
  float rate = ((1000.0 / deltaTime) * pulses) / FLOW_CALIBRATION;
  unsigned int ml = (rate / 60.0) * 1000;  // Convert to mL/sec

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    flowData.rate = rate;
    flowData.milliLitres = ml;
    flowData.totalML += ml;
    xSemaphoreGive(xMutex);
  }
}

void readPressureSensor() {
  // Read 12-bit ADC
  int rawADC = analogRead(PRESSURE_ADC_PIN);
  float adcVoltage = (rawADC / 4095.0) * 3.3;

  // Compensate for voltage divider
  float sensorVoltage = adcVoltage / VOLTAGE_DIVIDER;

  // Convert to pressure (0.5-4.5V → 0-8 bar)
  float bar = (sensorVoltage - 0.5) * (8.0 / 4.0);
  if (bar < 0) bar = 0;

  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    pressureData.bar = bar;
    pressureData.mpa = bar * 0.1;  // 1 bar = 0.1 MPa
    xSemaphoreGive(xMutex);
  }
}

void readRPMSensor(unsigned long deltaTime) {
  static portMUX_TYPE rpmMux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&rpmMux);
  unsigned long pulses = rpmData.pulseCount;
  rpmData.pulseCount = 0;
  portEXIT_CRITICAL(&rpmMux);

  // === Pulse Count (นับ pulse ต่อช่วงเวลา) ===
  float rpmCount = (pulses / (float)RPM_PULSES_PER_REV) * (60000.0 / deltaTime);

  // === Moving Average ===
  rpmBuffer[rpmBufIndex] = rpmCount;
  rpmBufIndex = (rpmBufIndex + 1) % RPM_AVG_SIZE;

  float sum = 0.0;
  for (int i = 0; i < RPM_AVG_SIZE; i++) sum += rpmBuffer[i];
  float rpmAvg = sum / RPM_AVG_SIZE;

  if (xSemaphoreTake(xMutex, pdMS_TO_TICKS(100))) {
    rpmData.rpmCount = rpmCount;  // ค่าดิบ (ใช้ใน Cloud / Teleplot)
    rpmData.rpmAvg   = rpmAvg;   // ค่าเฉลี่ย (ใช้บน LCD)
    xSemaphoreGive(xMutex);
  }
}

// ============================================================================
// DISPLAY FUNCTIONS
// ============================================================================

void updateLCD() {
  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    char line1[21], line2[21], line3[21], line4[21];

    // Line 1: Title
    snprintf(line1, sizeof(line1),
             "  Generator Monitor   ");  // 20 chars

    // Line 2: Voltage & Current (แสดง --- ถ้า PZEM อ่านไม่ได้)
    if (powerData.valid) {
      snprintf(line2, sizeof(line2),
               "V:%.1fV   I:%.2fA   ",
               powerData.voltage,
               powerData.current);
    } else {
      snprintf(line2, sizeof(line2),
               "V:---V   I:---A   ");
    }

    // Line 3: Power & Pressure (แสดง --- ถ้า PZEM อ่านไม่ได้)
    if (powerData.valid) {
      snprintf(line3, sizeof(line3),
               "P:%.2fW    %.1fbar   ",
               powerData.power,
               pressureData.bar);
    } else {
      snprintf(line3, sizeof(line3),
               "P:---W    %.1fbar   ",
               pressureData.bar);
    }

    // Line 4: Flowrate & RPM (ใช้ค่าเฉลี่ย)
    snprintf(line4, sizeof(line4),
             "F:%.1fL/m %4drpm ",
             flowData.rate,
             (int)rpmData.rpmAvg);

    // แสดงผลบน LCD
    lcd.setCursor(0, 0); lcd.print(line1);
    lcd.setCursor(0, 1); lcd.print(line2);
    lcd.setCursor(0, 2); lcd.print(line3);
    lcd.setCursor(0, 3); lcd.print(line4);

    xSemaphoreGive(xMutex);
  }
}

void printTeleplot() {
  if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
    // Teleplot format: >variable_name:value
    Serial.printf(">voltage:%.2f\n", powerData.voltage);
    Serial.printf(">current:%.3f\n", powerData.current);
    Serial.printf(">power:%.1f\n", powerData.power);
    Serial.printf(">energy:%.0f\n", powerData.energy);
    Serial.printf(">flow:%.2f\n", flowData.rate);
    Serial.printf(">pressure:%.2f\n", pressureData.bar);
    Serial.printf(">rpm_count:%.0f\n", rpmData.rpmCount);
    xSemaphoreGive(xMutex);
  }
}

// ============================================================================
// FREERTOS TASK FUNCTIONS
// ============================================================================

void TaskCloud(void *pvParameters) {
  for (;;) {
    digitalWrite(LED_POWER , HIGH);
    ArduinoCloud.update();

    if (WiFi.status() == WL_CONNECTED) {
      digitalWrite(LED_STATUS, HIGH);
    } else {
      digitalWrite(LED_STATUS, LOW);
    }

    if (xSemaphoreTake(xMutex, portMAX_DELAY)) {
      // Teleplot output
      Serial.printf(">voltage:%.2f\n", powerData.voltage);
      Serial.printf(">current:%.3f\n", powerData.current);
      Serial.printf(">flow:%.2f\n", flowData.rate);
      Serial.printf(">pressure:%.2f\n", pressureData.bar);
      Serial.printf(">rpm_count:%.0f\n", rpmData.rpmCount);
      volt = powerData.voltage;
      current = powerData.current;
      flow = flowData.rate;
      pressure = pressureData.bar;
      rpm = rpmData.rpmCount;  // ส่ง Pulse Count ขึ้น Cloud
      xSemaphoreGive(xMutex);
    }

    vTaskDelay(CLOUD_UPDATE_RATE / portTICK_PERIOD_MS);
  }
}

void TaskPZEM(void *pvParameters) {
  for (;;) {
    if (readPowerSensor()) {
      printTeleplot();
    }
    vTaskDelay(PZEM_READ_RATE / portTICK_PERIOD_MS);
  }
}

void TaskLCD(void *pvParameters) {
  for (;;) {
    updateLCD();
    vTaskDelay(LCD_UPDATE_RATE / portTICK_PERIOD_MS);
  }
}

void TaskFlow(void *pvParameters) {
  unsigned long lastTime = millis();

  for (;;) {
    unsigned long currentTime = millis();
    unsigned long deltaTime = currentTime - lastTime;

    if (deltaTime >= 1000) {
      readFlowSensor(deltaTime);
      lastTime = currentTime;
    }

    vTaskDelay(FLOW_READ_RATE / portTICK_PERIOD_MS);
  }
}

void TaskPressure(void *pvParameters) {
  for (;;) {
    readPressureSensor();
    vTaskDelay(PRESSURE_READ_RATE / portTICK_PERIOD_MS);
  }
}

void TaskRPM(void *pvParameters) {
  unsigned long lastTime = millis();

  for (;;) {
    unsigned long currentTime = millis();
    unsigned long deltaTime = currentTime - lastTime;

    if (deltaTime >= 1000) {
      readRPMSensor(deltaTime);
      lastTime = currentTime;
    }

    vTaskDelay(RPM_READ_RATE / portTICK_PERIOD_MS);
  }
}

// ============================================================================
// PZEM CONFIGURATION FUNCTIONS
// ============================================================================

void configurePZEMShunt(uint8_t slaveAddr, uint16_t shuntAddr) {
  uint16_t crc = 0xFFFF;
  uint8_t cmd = 0x06;
  uint16_t reg = 0x0003;

  crc = crc16_update(crc, slaveAddr);
  crc = crc16_update(crc, cmd);
  crc = crc16_update(crc, highByte(reg));
  crc = crc16_update(crc, lowByte(reg));
  crc = crc16_update(crc, highByte(shuntAddr));
  crc = crc16_update(crc, lowByte(shuntAddr));

  Serial.println("Configuring PZEM shunt...");
  Serial2.write(slaveAddr);
  Serial2.write(cmd);
  Serial2.write(highByte(reg));
  Serial2.write(lowByte(reg));
  Serial2.write(highByte(shuntAddr));
  Serial2.write(lowByte(shuntAddr));
  Serial2.write(lowByte(crc));
  Serial2.write(highByte(crc));

  delay(100);

  while (Serial2.available()) {
    Serial.print(Serial2.read(), HEX);
    Serial.print(" ");
  }
  Serial.println();
}

// ============================================================================
// HARDWARE INITIALIZATION
// ============================================================================

void initHardware() {
  // Serial
  Serial.begin(115200);
  delay(1500);

  // I2C + LCD
  Wire.begin();
  lcd.begin(20, 4);
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  // Flow Sensor
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowPulseISR, FALLING);

  // Hall Sensor (RPM)
  pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_SENSOR_PIN), rpmPulseISR, FALLING);

  // Pressure Sensor ADC
  analogReadResolution(12);
  analogSetPinAttenuation(PRESSURE_ADC_PIN, ADC_11db);

  //Pilot lamp
  pinMode(LED_STATUS , OUTPUT);
  pinMode(LED_POWER, OUTPUT);
  digitalWrite(LED_POWER , HIGH);
  digitalWrite(LED_STATUS , LOW);


  // PZEM-017  TX=17 RX=16
  Serial2.begin(9600, SERIAL_8N2, 16, 17);
  configurePZEMShunt(PZEM_SLAVE_ADDR, PZEM_SHUNT_ADDR);
  pzemNode.begin(PZEM_SLAVE_ADDR, Serial2);
  delay(1000);
}

void initCloudConnection() {
  initProperties();
  ArduinoCloud.begin(ArduinoIoTPreferredConnection);
  setDebugMessageLevel(2);
  ArduinoCloud.printDebugInfo();
}

void createTasks() {
  xMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(TaskCloud, "Cloud", 10000, NULL, 1, &hTaskCloud, 0);
  xTaskCreatePinnedToCore(TaskPZEM, "PZEM", 10000, NULL, 2, &hTaskPZEM, 1);
  xTaskCreatePinnedToCore(TaskLCD, "LCD", 4096, NULL, 1, &hTaskLCD, 1);  // Core 1 (same as Wire.begin)
  xTaskCreatePinnedToCore(TaskFlow, "Flow", 4096, NULL, 2, &hTaskFlow, 1);
  xTaskCreatePinnedToCore(TaskPressure, "Pressure", 4096, NULL, 2, &hTaskPressure, 1);
  xTaskCreatePinnedToCore(TaskRPM, "RPM", 4096, NULL, 2, &hTaskRPM, 1);
}

// ============================================================================
// MAIN FUNCTIONS
// ============================================================================

void setup() {
  initHardware();
  initCloudConnection();
  createTasks();
}

void loop() {
  // Empty - all work done by FreeRTOS tasks
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
