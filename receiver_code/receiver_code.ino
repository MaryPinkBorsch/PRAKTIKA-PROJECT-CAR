#include <esp_now.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// receiver4

// Левый мотор
#define IN1 25
#define IN2 26
#define ENA 15 // Пин ШИМ для левого мотора

// Правый мотор
#define IN3 27
#define IN4 14
#define ENB 12 // Пин ШИМ для правого мотора

// Настройки ШИМ для нового ядра ESP32 (3.x.x)
#define PWM_FREQ 5000 // Частота ШИМ (5 кГц)
#define PWM_RES 8     // Разрешение ШИМ (8 бит: от 0 до 255)

// В новом ядре ESP32 каналы (CH_LEFT/CH_RIGHT) больше не нужны,
// система распределяет их автоматически.

const int ledPin = 2;
volatile unsigned long lastcommandTime = 0;
const unsigned long TIMEOUT_MS = 500;

typedef struct
{
  int xValue;
  int yValue;
  int buttonState;
  int messageId;
} JoystickData;

JoystickData receivedData;
volatile bool newDataAvailable = false;

TaskHandle_t receiveTaskHandle = NULL;
TaskHandle_t printTaskHandle = NULL;
QueueHandle_t dataQueue;

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len)
{
  if (len == sizeof(JoystickData))
  {
    JoystickData tempData;
    memcpy(&tempData, incomingData, sizeof(tempData));
    xQueueSendFromISR(dataQueue, &tempData, NULL);
  }
}

void receiveTask(void *pvParameters)
{
  JoystickData tempData;
  while (1)
  {
    if (xQueueReceive(dataQueue, &tempData, portMAX_DELAY) == pdTRUE)
    {
      portDISABLE_INTERRUPTS();
      receivedData = tempData;
      newDataAvailable = true;
      portENABLE_INTERRUPTS();
      xTaskNotifyGive(printTaskHandle);
    }
  }
}

// Изменяем скорость через новую функцию ledcWrite, которая теперь принимает ПИН напрямую
void setMotorSpeed(int speedLeft, int speedRight)
{
  // Статические переменные хранят реальную скорость моторов между вызовами функции
  static float currentLeft = 0.0;
  static float currentRight = 0.0;

  // КОЭФФИЦИЕНТ ПЛАВНОСТИ:
  // 1.0 — мгновенно (как было), 0.1 — плавно, 0.02 — ОЧЕНЬ медленный разгон.
  const float STEP = 0.2;

  // Ограничиваем входные значения от греха подальше
  speedLeft = constrain(speedLeft, 0, 255);
  speedRight = constrain(speedRight, 0, 255);

  // Плавно подтягиваем текущую скорость к целевой
  currentLeft += ((float)speedLeft - currentLeft) * STEP;
  currentRight += ((float)speedRight - currentRight) * STEP;

  // Если разница микроскопическая, приравниваем, чтобы мотор не пищал на месте
  if (abs(currentLeft - speedLeft) < 1.0)
    currentLeft = speedLeft;
  if (abs(currentRight - speedRight) < 1.0)
    currentRight = speedRight;

  // Отправляем сглаженное значение на пины ESP32
  ledcWrite(ENA, (int)currentLeft);
  ledcWrite(ENB, (int)currentRight);
}

void GoForward(int speed)
{
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  setMotorSpeed(speed, speed);
  Serial.print("FORWARD - SPEED: ");
  Serial.println(speed);
}

void GoBack(int speed)
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  setMotorSpeed(speed, speed);
  Serial.print("BACKWARD - SPEED: ");
  Serial.println(speed);
}

void GoLeft(int speed)
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
  setMotorSpeed(speed, speed);
  Serial.print("LEFT TURN - SPEED: ");
  Serial.println(speed);
}

void GoRight(int speed)
{
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
  setMotorSpeed(speed, speed);
  Serial.print("RIGHT TURN - SPEED: ");
  Serial.println(speed);
}

void Stop()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  setMotorSpeed(0, 0);
  Serial.println("STOP");
}

void controlFromJoystick(JoystickData data)
{
  int x = data.xValue;
  int y = data.yValue;

  const int DEADZONE = 250;
  const int CENTER = 2048;

  bool xCentered = (x > CENTER - DEADZONE && x < CENTER + DEADZONE);
  bool yCentered = (y > CENTER - DEADZONE && y < CENTER + DEADZONE);

  if (data.buttonState == LOW)
  {
    Stop();
    Serial.println("⚠️ EMERGENCY STOP by button!");
    return;
  }

  if (yCentered && xCentered)
  {
    Stop();
  }
  else if (!yCentered)
  {
    if (y < CENTER - DEADZONE)
    {
      int speed = map(y, CENTER - DEADZONE, 0, 0, 255);
      GoBack(speed);
    }
    else if (y > CENTER + DEADZONE)
    {
      int speed = map(y, CENTER + DEADZONE, 4095, 0, 255);
      GoForward(speed);
    }
  }
  else if (!xCentered)
  {
    if (x < CENTER - DEADZONE)
    {
      int speed = map(x, CENTER - DEADZONE, 0, 0, 255);
      GoLeft(speed);
    }
    else if (x > CENTER + DEADZONE)
    {
      int speed = map(x, CENTER + DEADZONE, 4095, 0, 255);
      GoRight(speed);
    }
  }
}

void printTask(void *pvParameters)
{
  JoystickData localData;
  while (1)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (newDataAvailable)
    {
      lastcommandTime = millis();
      digitalWrite(ledPin, HIGH);

      portDISABLE_INTERRUPTS();
      localData = receivedData;
      newDataAvailable = false;
      portENABLE_INTERRUPTS();

      Serial.println("=================================");
      Serial.print("📦 Message ID: ");
      Serial.println(localData.messageId);
      Serial.print("🕹️  X-Axis: ");
      Serial.println(localData.xValue);
      Serial.print("🕹️  Y-Axis: ");
      Serial.println(localData.yValue);
      Serial.print("🔘 Button: ");
      Serial.println(localData.buttonState == LOW ? "PRESSED" : "RELEASED");

      controlFromJoystick(localData);
      Serial.println("=================================\n");

      vTaskDelay(pdMS_TO_TICKS(50));
    }
    digitalWrite(ledPin, LOW);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(100);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);

  // ИСПРАВЛЕНО ДЛЯ ESP32 CORE 3.X.X:
  // Вместо ledcSetup и ledcAttachPin теперь используется одна функция ledcAttach!
  ledcAttach(ENA, PWM_FREQ, PWM_RES);
  ledcAttach(ENB, PWM_FREQ, PWM_RES);

  Stop();

  Serial.println("🔧 ESP32 Receiver Starting (v3.x.x compatible)...");
  Serial.println("✅ PWM Speed Control INITIALIZED on pins 15 (ENA) and 2 (ENB)");
  Serial.println("========================================================");

  dataQueue = xQueueCreate(10, sizeof(JoystickData));

  WiFi.mode(WIFI_STA);
  WiFi.begin();
  delay(500);

  if (esp_now_init() != ESP_OK)
  {
    Serial.println("❌ Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
  lastcommandTime = millis();

  xTaskCreatePinnedToCore(receiveTask, "Receive Task", 4096, NULL, 2, &receiveTaskHandle, 0);
  xTaskCreatePinnedToCore(printTask, "Print Task", 4096, NULL, 1, &printTaskHandle, 1);
}

void loop()
{
  vTaskDelay(pdMS_TO_TICKS(50));

  if (millis() - lastcommandTime > TIMEOUT_MS)
  {
    Stop();
  }
}
