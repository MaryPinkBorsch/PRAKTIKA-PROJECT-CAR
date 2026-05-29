#include <esp_now.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
//ver7

// Левый мотор
#define IN1 25
#define IN2 26
#define ENA 15 // Пин ШИМ для левого мотора

// Правый мотор
#define IN3 27
#define IN4 14
#define ENB 12  // Пин ШИМ для правого мотора

// Настройки ШИМ для нового ядра ESP32 (3.x.x)
#define PWM_FREQ 5000       // Частота ШИМ (5 кГц)
#define PWM_RES 8           // Разрешение ШИМ (8 бит: от 0 до 255)

const int ledPin = 2; 
volatile unsigned long lastcommandTime = 0;
const unsigned long TIMEOUT_MS = 500; 

typedef struct {
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

void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingData, int len) {
  if (len == sizeof(JoystickData)) {
    JoystickData tempData;
    memcpy(&tempData, incomingData, sizeof(tempData));
    xQueueSendFromISR(dataQueue, &tempData, NULL);
  }
}

void receiveTask(void *pvParameters) {
  JoystickData tempData;
  while (1) {
    if (xQueueReceive(dataQueue, &tempData, portMAX_DELAY) == pdTRUE) {
      portDISABLE_INTERRUPTS();
      receivedData = tempData;
      newDataAvailable = true;
      portENABLE_INTERRUPTS();
      xTaskNotifyGive(printTaskHandle);
    }
  }
}

// Изменяем скорость через новую функцию ledcWrite
void setMotorSpeed(int speedLeft, int speedRight) {
  // Статические переменные хранят реальную скорость моторов между вызовами функции
  static float currentLeft = 0.0;
  static float currentRight = 0.0;

  // КОЭФФИЦИЕНТ ПЛАВНОСТИ
  const float STEP = 0.2; 

  // Ограничиваем входные значения
  speedLeft = constrain(speedLeft, 0, 255);
  speedRight = constrain(speedRight, 0, 255);

  // Плавно подтягиваем текущую скорость к целевой
  currentLeft  += ((float)speedLeft - currentLeft) * STEP;
  currentRight += ((float)speedRight - currentRight) * STEP;

  // Если разница микроскопическая, приравниваем
  if (abs(currentLeft - speedLeft) < 1.0) currentLeft = speedLeft;
  if (abs(currentRight - speedRight) < 1.0) currentRight = speedRight;

  // Отправляем сглаженное значение на пины ESP32
  ledcWrite(ENA, (int)currentLeft);
  ledcWrite(ENB, (int)currentRight);
}

void GoForward(int speedLeft, int speedRight) {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
  setMotorSpeed(speedLeft, speedRight);
  Serial.print("FORWARD - L:"); Serial.print(speedLeft); 
  Serial.print(" R:"); Serial.println(speedRight);
}

void GoBack(int speedLeft, int speedRight) {
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
  setMotorSpeed(speedLeft, speedRight);
  Serial.print("BACKWARD - L:"); Serial.print(speedLeft); 
  Serial.print(" R:"); Serial.println(speedRight);
}

void TurnLeft(int speed) {
  digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);  // Левый мотор назад
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  // Правый мотор вперед
  setMotorSpeed(speed, speed);
  Serial.print("TURN LEFT - SPEED: "); Serial.println(speed);
}

void TurnRight(int speed) {
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);  // Левый мотор вперед
  digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);  // Правый мотор назад
  setMotorSpeed(speed, speed);
  Serial.print("TURN RIGHT - SPEED: "); Serial.println(speed);
}

void Stop() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  setMotorSpeed(0, 0); 
  Serial.println("STOP");
}

void controlFromJoystick(JoystickData data) {
  int x = data.xValue;
  int y = data.yValue;

  const int DEADZONE = 250;
  const int CENTER = 2048;  
  const int MAX_VALUE = 4095;
  
  // Коэффициент чувствительности поворота (0.3 = 30% от максимального поворота)
  const float TURN_SENSITIVITY = 0.3;

  bool xCentered = (x > CENTER - DEADZONE && x < CENTER + DEADZONE);
  bool yCentered = (y > CENTER - DEADZONE && y < CENTER + DEADZONE);

  // Аварийная остановка по кнопке
  if (data.buttonState == LOW) {
    Stop();
    Serial.println("⚠️ EMERGENCY STOP by button!");
    return;
  }

  // Если джойстик в центре - остановка
  if (yCentered && xCentered) {
    Stop();
    return;
  }

  // ОСОБЫЙ СЛУЧАЙ: Поворот на месте (только X отклонен, Y в нейтрали)
  if (yCentered && !xCentered) {
    if (x < CENTER - DEADZONE) {
      // Поворот налево на месте
      int speed = map(x, CENTER - DEADZONE, 0, 0, 255);
      TurnLeft(speed);
    } else if (x > CENTER + DEADZONE) {
      // Поворот направо на месте
      int speed = map(x, CENTER + DEADZONE, MAX_VALUE, 0, 255);
      TurnRight(speed);
    }
    return;
  }

  // Обычное движение (Y не в нейтрали)
  // Вычисляем базовую скорость из оси Y (перед/назад)
  int baseSpeed = 0;
  bool isForward = true;
  
  if (y < CENTER - DEADZONE) {
    // Движение назад
    baseSpeed = map(y, CENTER - DEADZONE, 0, 0, 255);
    isForward = false;
  } else if (y > CENTER + DEADZONE) {
    // Движение вперед
    baseSpeed = map(y, CENTER + DEADZONE, MAX_VALUE, 0, 255);
    isForward = true;
  }

  // Вычисляем фактор поворота из оси X (влево/вправо)
  int turnFactor = 0;
  if (x < CENTER - DEADZONE) {
    // Поворот налево (отрицательное значение)
    turnFactor = map(x, CENTER - DEADZONE, 0, 0, -255);
  } else if (x > CENTER + DEADZONE) {
    // Поворот направо (положительное значение)
    turnFactor = map(x, CENTER + DEADZONE, MAX_VALUE, 0, 255);
  }

  // Применяем чувствительность поворота
  turnFactor = turnFactor * TURN_SENSITIVITY;

  // Рассчитываем скорости для левого и правого мотора
  int speedLeft = baseSpeed;
  int speedRight = baseSpeed;

  // Корректируем скорости для поворота
  if (turnFactor > 0) {
    // Поворот направо: уменьшаем скорость правого мотора
    speedRight = constrain(baseSpeed - turnFactor, 0, 255);
  } else if (turnFactor < 0) {
    // Поворот налево: уменьшаем скорость левого мотора
    speedLeft = constrain(baseSpeed + turnFactor, 0, 255); // turnFactor отрицательный
  }

  // Двигаемся с рассчитанными скоростями
  if (isForward) {
    GoForward(speedLeft, speedRight);
  } else {
    GoBack(speedLeft, speedRight);
  }
  
  // Выводим отладочную информацию
  Serial.print("BaseSpeed: "); Serial.print(baseSpeed);
  Serial.print(" TurnFactor: "); Serial.print(turnFactor);
  Serial.print(" -> L:"); Serial.print(speedLeft);
  Serial.print(" R:"); Serial.println(speedRight);
}

void printTask(void *pvParameters) {
  JoystickData localData;
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (newDataAvailable) {
      lastcommandTime = millis();
      digitalWrite(ledPin, HIGH);

      portDISABLE_INTERRUPTS();
      localData = receivedData;
      newDataAvailable = false;
      portENABLE_INTERRUPTS();

      Serial.println("=================================");
      Serial.print("📦 Message ID: "); Serial.println(localData.messageId);
      Serial.print("🕹️  X-Axis: "); Serial.println(localData.xValue);
      Serial.print("🕹️  Y-Axis: "); Serial.println(localData.yValue);
      Serial.print("🔘 Button: "); Serial.println(localData.buttonState == LOW ? "PRESSED" : "RELEASED");

      controlFromJoystick(localData);
      Serial.println("=================================\n");

      vTaskDelay(pdMS_TO_TICKS(50));
    }
    digitalWrite(ledPin, LOW);
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(ledPin, OUTPUT);
  digitalWrite(ledPin, LOW);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);

  // Инициализация ШИМ для ESP32 CORE 3.X.X
  ledcAttach(ENA, PWM_FREQ, PWM_RES);
  ledcAttach(ENB, PWM_FREQ, PWM_RES);

  Stop(); 

  Serial.println("🔧 ESP32 Receiver Starting (v3.x.x compatible)...");
  Serial.println("✅ PWM Speed Control INITIALIZED on pins 15 and 12");
  Serial.println("✅ Turn in place when Y is centered, X is deflected");
  Serial.println("✅ Diagonal movement with gentle turning enabled");
  Serial.println("========================================================");

  dataQueue = xQueueCreate(10, sizeof(JoystickData));

  WiFi.mode(WIFI_STA);
  WiFi.begin();
  delay(500);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(OnDataRecv);
  lastcommandTime = millis();

  xTaskCreatePinnedToCore(receiveTask, "Receive Task", 4096, NULL, 2, &receiveTaskHandle, 0);
  xTaskCreatePinnedToCore(printTask, "Print Task", 4096, NULL, 1, &printTaskHandle, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(50));

  if (millis() - lastcommandTime > TIMEOUT_MS) {
    Stop();
  }
}