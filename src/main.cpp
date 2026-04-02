#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Adafruit_AHTX0.h"
#include <stdarg.h>

#define DEBUG 1

#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5


Adafruit_AHTX0 aht;
Adafruit_Sensor *aht_humidity, *aht_temp;

SemaphoreHandle_t i2c_mutex; // I2C总线互斥锁

// ===================== 串口互斥锁 =====================
SemaphoreHandle_t serial_mutex;


void safe_printf(const char *format, ...)
{
  if (!DEBUG)
    return;

 
  xSemaphoreTake(serial_mutex, portMAX_DELAY);

  // 3. 处理可变参数（printf核心）
  char buffer[256]; // 打印缓冲区
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  // 4. 打印输出
  Serial.print(buffer);

  // 5. 解锁
  xSemaphoreGive(serial_mutex);
}

// ===================== AHT20任务 =====================
void aht20_task(void *pvParameters)
{
  // ========== vTaskDelayUntil 核心配置 ==========
  TickType_t last_wake_time;                          // 保存上次唤醒时间
  const TickType_t read_period = pdMS_TO_TICKS(1000); // 周期：1秒

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); // 初始化I2C总线

  safe_printf("Adafruit AHT10/AHT20 test!\n");

  if (!aht.begin())
  {
    safe_printf("Failed to find AHT10/AHT20 chip\n");
    while (1)
    {
      vTaskDelay(pdMS_TO_TICKS(10)); // 延迟10毫秒，避免过多输出
    }
  }

  safe_printf("AHT10/AHT20 Found!\n");

  aht_temp = aht.getTemperatureSensor();
#ifdef DEBUG
  aht_temp->printSensorDetails();
#endif
  aht_humidity = aht.getHumiditySensor();
#ifdef DEBUG
  aht_humidity->printSensorDetails();
#endif

  last_wake_time = xTaskGetTickCount(); // 初始化：记录系统当前时间
  while(1){

    sensors_event_t humidity;
    sensors_event_t temp;

    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    aht_humidity->getEvent(&humidity);
    aht_temp->getEvent(&temp);
    xSemaphoreGive(i2c_mutex);

    // 温湿度格式化打印
    safe_printf("\t\tTemperature %.2f deg C\n", temp.temperature);
    safe_printf("\t\tHumidity: %.2f %% rH\n", humidity.relative_humidity);
    safe_printf("\t\tTemperature: %.2f degrees C\n", temp.temperature);

    vTaskDelayUntil(&last_wake_time, read_period); // 以固定周期唤醒任务
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial) delay(10);

  // 1. 初始化串口互斥锁
  serial_mutex = xSemaphoreCreateMutex();

  // 2. 初始化I2C总线互斥锁
  i2c_mutex = xSemaphoreCreateMutex();

  // 2. 创建 AHT20 任务
  xTaskCreate(
      aht20_task,   // 任务函数
      "AHT20_Task", // 任务名称
      2048,         // 栈大小
      NULL,         // 参数
      1,            // 优先级
      NULL          // 句柄
  );
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(10));
}


