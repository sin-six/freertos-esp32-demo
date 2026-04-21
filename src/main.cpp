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
#include <lvgl.h>
#include "cst816t.h" // capacitive touch
#include <TFT_eSPI.h>

// 添加 GUI-Guider 生成的头文件
#include "gui_guider.h"
#include "events_init.h"

#define DEBUG 1
#define SLEEP_MODE 0 // 0: 不睡眠，1: 轻睡眠


#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5

#define TP_SDA 4
#define TP_SCL 5
#define TP_RST 8
#define TP_IRQ 9

#define TFT_BL 2
#define TFT_BACKLIGHT_ON HIGH

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 280

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

// ===================== WiFi互斥锁 =====================
SemaphoreHandle_t wifi_mutex;

QueueHandle_t data_queue; // 传感器数据队列
QueueHandle_t ui_queue;// UI更新队列

// ===================== MQTT客户端 =====================
WiFiClient espClient;
PubSubClient client(espClient);

// ===================== 触摸全局结构体 =====================
cst816t touch(Wire, TP_RST, TP_IRQ);

TFT_eSPI tft = TFT_eSPI();
lv_ui guider_ui;

// 原始坐标范围（需要根据实际触摸屏校准后确定）
#define RAW_X_MIN 200  // 左上角 X 最小值
#define RAW_X_MAX 3950 // 右下角 X 最大值
#define RAW_Y_MIN 150  // 左上角 Y 最小值
#define RAW_Y_MAX 3850 // 右下角 Y 最大值

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

  Serial.print(buffer);

  xSemaphoreGive(serial_mutex);
}

void wifi_connect()
{
  if (xSemaphoreTake(wifi_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      xSemaphoreGive(wifi_mutex); 
      return;
    }
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

  xSemaphoreGive(wifi_mutex);
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

// ===================== LVGL显示刷新回调 =====================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

// 触摸读取回调
// ========== 触摸读取回调 ==========
void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
  xSemaphoreTake(i2c_mutex, portMAX_DELAY);
  bool touched = touch.available();
  if (touched)
  {
    // 临时打印原始坐标（调试用）
    safe_printf("raw: x=%d, y=%d\n", touch.x, touch.y);

    // 线性映射到屏幕分辨率
    data->point.x = map(touch.x, RAW_X_MIN, RAW_X_MAX, 0, SCREEN_WIDTH - 1);
    data->point.y = map(touch.y, RAW_Y_MIN, RAW_Y_MAX, 0, SCREEN_HEIGHT - 1);

    // 边界裁剪，防止超出屏幕范围
    if (data->point.x >= SCREEN_WIDTH)
      data->point.x = SCREEN_WIDTH - 1;
    if (data->point.y >= SCREEN_HEIGHT)
      data->point.y = SCREEN_HEIGHT - 1;

    data->state = LV_INDEV_STATE_PR;
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
  xSemaphoreGive(i2c_mutex);
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
    xQueueOverwrite(ui_queue, &data); // 发送数据到 UI 队列，覆盖之前的数据

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

void ui_lvgl_task(void *pvParameters)
{
  // 这里可以添加LVGL的UI更新代码
  // 屏幕初始化
  tft.begin();
  tft.setRotation(0);

  // pinMode(TFT_BL, OUTPUT);
  // digitalWrite(TFT_BL, HIGH);

  ledcSetup(0, 5000, 8); // 通道0，频率5kHz，分辨率8位
  ledcAttachPin(TFT_BL, 0); // 将TFT_BL引脚
  ledcWrite(0, 255); // 设置亮度为最大值

  touch.begin(mode_change);

  lv_init();

  // 分配显示缓冲区（一行或多行）
  static lv_disp_draw_buf_t draw_buf;
  static lv_color_t buf[SCREEN_WIDTH * 10]; // 10 行缓冲区
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 10);

  // 注册显示驱动
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.draw_buf = &draw_buf;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  lv_disp_drv_register(&disp_drv);

  // 注册触摸输入
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // ========== 加载 GUI-Guider 生成的 UI ==========
  setup_ui(&guider_ui);
  events_init(&guider_ui);
  lv_scr_load(guider_ui.screen); // 加载主屏幕

  while (1)
  {
    SensorData_t latest; // 传感器数据结构体
    if (xQueueReceive(ui_queue, &latest, 0) == pdTRUE)
    {
      // 更新温度仪表盘（假设你有一个仪表盘指针对象）
      lv_meter_set_indicator_value(guider_ui.screen_1_meter_1,
                                   guider_ui.screen_1_meter_1_scale_0_ndline_0,
                                   latest.temperature);

      lv_meter_set_indicator_value(guider_ui.screen_2_meter_1,
                                   guider_ui.screen_2_meter_1_scale_0_ndline_0,
                                   latest.humidity);

      lv_meter_set_indicator_value(guider_ui.screen_3_meter_1,
                                   guider_ui.screen_3_meter_1_scale_0_ndline_0,
                                   latest.bmp_pressure);
    }

    lv_timer_handler(); // 处理 LVGL 任务
    vTaskDelay(pdMS_TO_TICKS(5)); // 每5ms更新一次UI
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

  // 初始化串口互斥锁
  serial_mutex = xSemaphoreCreateMutex();
  // 初始化I2C总线互斥锁
  i2c_mutex = xSemaphoreCreateMutex();
  // 初始化WiFi互斥锁
  wifi_mutex = xSemaphoreCreateMutex();

  // 创建传感器数据队列
  data_queue = xQueueCreate(10, sizeof(SensorData_t));
  // 创建lvgls数据队列
  ui_queue = xQueueCreate(1, sizeof(SensorData_t));

  wifi_connect();

  // 创建 sensor 任务
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
      NULL        // 句柄
  );

  xTaskCreate(
      ui_lvgl_task,   // 任务函数
      "UI_LVGL_Task", // 任务名称
      8192,           // 栈大小
      NULL,           // 参数
      6,              // 优先级
      NULL            // 句柄
  );
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}


