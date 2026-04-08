# freertos-esp32-demo
利用rtos实现各种传感器的采集，发送与显示

# ESP32 FreeRTOS + AHT20 & BMP280 数据采集 + 本地MQTT(Mosquitto)上报

## 项目简介
基于 ESP32 + FreeRTOS 实现的多任务温湿度采集系统
- AHT20 传感器定时采集温湿度
- BMP280 传感器定时采集气压
- FreeRTOS 多任务架构（传感器任务 + MQTT任务）
- 线程安全设计（互斥锁保护串口、I2C总线）
- 本地私有 Mosquitto MQTT 服务器（数据局域网传输，安全无串扰）
- MQTT 主动上报数据 + 订阅指令接收
- 任务优先级设置（传感器任务优先级高于 MQTT 任务）

## 硬件信息
- 主控：ESP32
- 传感器：AHT20 温湿度传感器 + BMP280 压力传感器
- I2C 引脚：
  - SDA：GPIO4
  - SCL：GPIO5

## 软件依赖
- Arduino-ESP32 核心
- Adafruit AHTX0 库
- PubSubClient MQTT库
- FreeRTOS（ESP32内置）

## 配置说明
### 1. WiFi 配置
```cpp
const char *ssid = "hello world-esp32";
const char *password = "interesting";

```
### 2. MQTT 配置
```cpp
const char *mqtt_server = "192.168.1.100";
const int mqtt_port = 1883;
const char *mqtt_user = "admin";
const char *mqtt_password = "admin";
```
### 3. 传感器配置
```cpp
#define I2C_SDA_PIN 4
#define I2C_SCL_PIN 5
```

## 功能说明
### 1. 传感器任务
- 定时采集温湿度数据
- 数据采集频率：1s