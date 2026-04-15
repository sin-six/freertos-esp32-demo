#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "Adafruit_AHTX0.h"
#include <stdarg.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_BMP280.h>
#include <Wire.h>
#include "esp_pm.h"
#include "esp_sleep.h"
#include <ArduinoOTA.h>

#define DEBUG 1
#define SLEEP_MODE 0 // 0: 不睡眠，1: 轻睡眠


#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5


typedef struct{
  float temperature; // AHT20温度
  float humidity;    // AHT20湿度

  float bmp_temperature; // BMP280温度
  float bmp_pressure;    // BMP280压力
} SensorData_t;

// =====================填写你的WIFI =====================
const char *ssid = "hello world-esp32";
const char *password = "interesting";

// ===================== MQTT服务器 =====================
const char *mqtt_server = "192.168.137.1";
const int mqtt_port = 1883;

// ===================== 目前没用上 =====================
const char *mqtt_user = "esp32_user";
const char *mqtt_pwd = "esp32_123456";

const char *mqtt_sub_top = "esp32/cmd";
const char *mqtt_pub_top = "esp32/aht20";

// ===================== 传感器 =====================
Adafruit_AHTX0 aht;
Adafruit_Sensor *aht_humidity, *aht_temp;

Adafruit_BMP280 bmp(&Wire); // use I2C interface
Adafruit_Sensor *bmp_temp = bmp.getTemperatureSensor();
Adafruit_Sensor *bmp_pressure = bmp.getPressureSensor();

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

  // 处理可变参数（printf核心）
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
#if SLEEP_MODE
    WiFi.setSleep(true);
    safe_printf("WiFi sleep enabled\n");
#endif
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
  client_id += String(esp_random(), HEX);

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

  // AHT20初始化
  safe_printf("Adafruit AHT10/AHT20 test!\n");
  uint8_t aht_reset_count = 0;
  bool aht_state = false;
  while (aht_reset_count <= 5)
  {
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    aht_state = aht.begin(&Wire);
    xSemaphoreGive(i2c_mutex);

    if(aht_state){
      safe_printf("Found AHT10/AHT20\n");
      break;
    }

    safe_printf("Could not find AHT10/AHT20 sensor, check wiring\n");
    aht_reset_count++;
    vTaskDelay(pdMS_TO_TICKS(500));

  }

  if (!aht_state)
  {
    safe_printf("AHT10/AHT20 initialization failed after multiple attempts!\n");
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP.restart(); // 重启系统，尝试重新初始化传感器
    //vTaskSuspend(NULL); // 挂起当前任务，停止执行
  }

    aht_temp = aht.getTemperatureSensor();
#if DEBUG
  aht_temp->printSensorDetails();
#endif
  aht_humidity = aht.getHumiditySensor();
#if DEBUG
  aht_humidity->printSensorDetails();
#endif

  // BMP280初始化
  unsigned status;
  xSemaphoreTake(i2c_mutex, portMAX_DELAY);
  status = bmp.begin();
  xSemaphoreGive(i2c_mutex);

  if(!status){
    safe_printf("Could not find a valid BMP280 sensor at 0x77, check wiring, address, sensor ID!\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    status = bmp.begin(0x76);
    xSemaphoreGive(i2c_mutex);
    if(!status){
      safe_printf("Could not find a valid BMP280 sensor at 0x76, check wiring, address, sensor ID!\n");
      vTaskSuspend(NULL);
    }
  }
  safe_printf("BMP280 Found!\n");

  /* Default settings from datasheet. */
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                  Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                  Adafruit_BMP280::SAMPLING_X16,    /* Pressure oversampling */
                  Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                  Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
#if DEBUG
  bmp_temp->printSensorDetails();
#endif
  
  last_wake_time = xTaskGetTickCount(); // 初始化：记录系统当前时间
  while(1){
    SensorData_t data; // 传感器数据结构体

    sensors_event_t bmp280_temp_event, bmp280_pressure_event;
    sensors_event_t humidity_event, temp_event;
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    aht_humidity->getEvent(&humidity_event);
    aht_temp->getEvent(&temp_event);
    bmp_temp->getEvent(&bmp280_temp_event);
    bmp_pressure->getEvent(&bmp280_pressure_event);
    xSemaphoreGive(i2c_mutex);

    data.temperature = temp_event.temperature;
    data.humidity = humidity_event.relative_humidity;
    data.bmp_pressure = bmp280_pressure_event.pressure;
    data.bmp_temperature = bmp280_temp_event.temperature;

    xQueueSend(data_queue, &data, portMAX_DELAY); // 发送数据到队列

    // 温湿度格式化打印
    safe_printf("\t\tTemperature %.2f deg C\n", data.temperature);
    safe_printf("\t\tHumidity: %.2f %% rH\n", data.humidity);
    safe_printf("\t\tBMP280.Temperature = %.2f *C\n", data.bmp_temperature);
    safe_printf("\t\tBMP280.Pressure = %.2f hPa\n", data.bmp_pressure);

    vTaskDelayUntil(&last_wake_time, read_period); // 以固定周期唤醒任务
  }
}

static TickType_t ota_delay = pdMS_TO_TICKS(500);
void ota_task(void *pvParameters)
{
  ArduinoOTA.onStart([]()
                      { safe_printf("OTA start\n");
                         ota_delay = pdMS_TO_TICKS(100); // OTA开始后加快循环频率，快速响应OTA事件
                         WiFi.setSleep(false); // OTA过程中关闭WiFi睡眠，确保连接稳定
                      });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        {
    static unsigned int last_pct = 0;
    unsigned int pct = progress / (total / 100);
    if (pct != last_pct) {
        last_pct = pct;
        safe_printf("OTA progress: %u%%\n", pct);
    } });
  ArduinoOTA.onEnd([]()
                   { safe_printf("OTA end\n"); 

                     ota_delay = pdMS_TO_TICKS(500); // OTA结束后恢复循环频率
                      WiFi.setSleep(true); // OTA结束后重新启用WiFi睡眠
                  });
  ArduinoOTA.onError([](ota_error_t error)
                     { safe_printf("OTA error: %d\n", error);
                        ota_delay = pdMS_TO_TICKS(500);
                        WiFi.setSleep(true); // OTA出错后恢复WiFi睡眠
                    });
  ArduinoOTA.begin();
  safe_printf("ArduinoOTA ready\n");

  while (1)
  {
    wifi_connect();
    ArduinoOTA.handle();
    vTaskDelay(ota_delay);
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

    if (xQueueReceive(data_queue, &data, pdMS_TO_TICKS(500)) == pdPASS)
    {
      // 温湿度格式化打印
      safe_printf("\t\tmqtt_Temperature %.2f deg C\n", data.temperature);
      safe_printf("\t\tmqtt_Humidity: %.2f %% rH\n", data.humidity);
      safe_printf("\t\tmqtt_BMP280.Temperature = %.2f *C\n", data.bmp_temperature);
      safe_printf("\t\tmqtt_BMP280.Pressure = %.2f hPa\n", data.bmp_pressure);

      char msg[128];
      snprintf(msg, sizeof(msg), "{\"temperature\": %.2f, \"humidity\": %.2f, \"bmp_temperature\": %.2f, \"bmp_pressure\": %.2f}", data.temperature, data.humidity, data.bmp_temperature, data.bmp_pressure);
      client.publish(mqtt_pub_top, msg);
      safe_printf("MQTT message published: %s\n", msg);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial) delay(10);

#if SLEEP_MODE 
  esp_pm_config_esp32c3_t pm_config = {
    .max_freq_mhz = 160,
    .min_freq_mhz = 40,
    .light_sleep_enable = false // 轻睡眠模式关闭
  };
  if(esp_pm_configure(&pm_config) != ESP_OK ) {
    safe_printf("Failed to configure power management\n");
  }
  else {
    safe_printf("Power management configured\n");
  }
#endif
  

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); // 初始化I2C总线

  // 1. 初始化串口互斥锁
  serial_mutex = xSemaphoreCreateMutex();

  // 2. 初始化I2C总线互斥锁
  i2c_mutex = xSemaphoreCreateMutex();

  // 3. 创建传感器数据队列
  data_queue = xQueueCreate(10, sizeof(SensorData_t));
  
  // 4. 连接WiFi
  wifi_connect();
  
  // 2. 创建 sensor 任务
  xTaskCreate(
      sensor_task,   // 任务函数
      "Sensor_Task", // 任务名称
      4096,          // 栈大小
      NULL,          // 参数
      9,             // 优先级
      NULL           // 句柄
  );

  xTaskCreate(
      mqtt_task,    // 任务函数
      "MQTT_Task",  // 任务名称
      4096,         // 栈大小
      NULL,         // 参数
      8,            // 优先级
      NULL          // 句柄
  );

  xTaskCreate(
      ota_task,   // 任务函数
      "OTA_Task", // 任务名称
      4096,       // 栈大小
      NULL,       // 参数
      7,          // 优先级
      NULL);      // 句柄
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}


