# ESP32 Smart Room Energy Optimization


The system uses a distributed architecture with three main nodes:

**AI Camera Node**: ESP32-S3 + OV3660, running a quantized Shallow CNN model with TensorFlow Lite Micro to recognize four room states: Empty, Reading, Sleep, and Active.

**Central Node**: ESP32-C6, responsible for system coordination, automatic control policy, ESP-NOW communication, Wi-Fi, and MQTT.

**Actuator Node**: ESP32-C6, controlling relays and air conditioner IR signals, reading RTC and load feedback, and reporting device status.

A Flutter mobile application is used to monitor the system and switch between AUTO and MANUAL control through MQTT.

## Main Focus

ESP32-C6, ESP32-S3, ESP-IDF, ESP-NOW, TensorFlow Lite Micro, MQTT, Mosquitto, Flutter, DS3231, IR control, and load detection.
