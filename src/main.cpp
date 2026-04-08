#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Adafruit_AHTX0.h"
#include <stdarg.h>
#include <WiFi.h>
#include <PubSubClient.h>

#define DEBUG 1

#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5


typedef struct{
  float temperature; // AHT20温度
  float humidity;    // AHT20湿度
} SensorData_t;

// =====================填写你的WIFI =====================
const char *ssid = "hello world-esp32";
const char *password = "interesting";

// ===================== MQTT服务器 =====================
const char *mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;

//目前没用上
const char *mqtt_user = "esp32_user";
const char *mqtt_pwd = "esp32_123456";

const char *mqtt_sub_top = "esp32/cmd";
const char *mqtt_pub_top = "esp32/aht20";

Adafruit_AHTX0 aht;
Adafruit_Sensor *aht_humidity, *aht_temp;

// ===================== I2C互斥锁 =====================
SemaphoreHandle_t i2c_mutex; // I2C总线互斥锁

// ===================== 串口互斥锁 =====================
SemaphoreHandle_t serial_mutex;

QueueHandle_t data_queue; // 传感器数据队列

// ===================== MQTT客户端 =====================
WiFiClient espClient;
PubSubClient client(espClient);


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

void wifi_connect()
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  safe_printf("WiFi linking...\n");
  WiFi.begin(ssid, password);

  // 最多等 10 秒，超时就退出，不卡死系统
  char wifi_status = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_status < 100)
  {
    vTaskDelay(pdMS_TO_TICKS(100)); // 延迟100毫秒，避免过多输出
    safe_printf(".");
    wifi_status++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    safe_printf("\nWiFi connected!\n");
    safe_printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  }else{
    safe_printf("\nWiFi connect failed!\n");
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  // 处理MQTT消息的回调函数
  safe_printf("MQTT message received on topic: %s\n", topic);
  safe_printf("Message payload: %.*s\n", length, payload);
}

void mqtt_connect()
{
  if(client.connected())
    return;

  static uint32_t last_retry = 0;
  if(millis() - last_retry < 3000)
    return;

  last_retry = millis();

  String client_id = "ESP32Client-";
  client_id += String(random(0xffff), HEX);

  safe_printf("MQTT connecting...\n");
  if (client.connect(client_id.c_str()))
  {
    safe_printf("MQTT connected!\n");
    client.subscribe(mqtt_sub_top);
  } else {
    safe_printf("MQTT connect failed, rc=%d\n", client.state());
  }
}


// ===================== 传感器任务 =====================
void sensor_task(void *pvParameters)
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
    SensorData_t data; // 传感器数据结构体

    sensors_event_t humidity_event, temp_event;
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    aht_humidity->getEvent(&humidity_event);
    aht_temp->getEvent(&temp_event);
    xSemaphoreGive(i2c_mutex);

    data.temperature = temp_event.temperature;
    data.humidity = humidity_event.relative_humidity;

    xQueueSend(data_queue, &data, portMAX_DELAY); // 发送数据到队列

    // 温湿度格式化打印
    safe_printf("\t\tTemperature %.2f deg C\n", data.temperature);
    safe_printf("\t\tHumidity: %.2f %% rH\n", data.humidity);

    vTaskDelayUntil(&last_wake_time, read_period); // 以固定周期唤醒任务
  }
}

void mqtt_task(void *pvParameters)
{
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  wifi_connect();

  while(1){
    SensorData_t data; // 传感器数据结构体
    wifi_connect();
    mqtt_connect();
    client.loop();

    if (xQueueReceive(data_queue, &data, pdMS_TO_TICKS(100)) == pdPASS)
    {
      // 温湿度格式化打印
      safe_printf("\t\tTemperature %.2f deg C\n", data.temperature);
      safe_printf("\t\tHumidity: %.2f %% rH\n", data.humidity);

      char msg[64];
      snprintf(msg, sizeof(msg), "{\"temperature\": %.2f, \"humidity\": %.2f}", data.temperature, data.humidity);
      client.publish(mqtt_pub_top, msg);
      safe_printf("MQTT message published: %s\n", msg);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial) delay(10);

  // 1. 初始化串口互斥锁
  serial_mutex = xSemaphoreCreateMutex();

  // 2. 初始化I2C总线互斥锁
  i2c_mutex = xSemaphoreCreateMutex();

  // 3. 创建传感器数据队列
  data_queue = xQueueCreate(5, sizeof(SensorData_t));
  
  // 2. 创建 sensor 任务
  xTaskCreate(
      sensor_task,  // 任务函数
      "Sensor_Task", // 任务名称
      2048,         // 栈大小
      NULL,         // 参数
      9,            // 优先级
      NULL          // 句柄
  );

  xTaskCreate(
      mqtt_task,  // 任务函数
      "MQTT_Task", // 任务名称
      4096,         // 栈大小
      NULL,         // 参数
      8,            // 优先级
      NULL          // 句柄
  );



}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(10));
}


