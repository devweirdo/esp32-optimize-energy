#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "cJSON.h"

#include "espnow_protocol.h"

static const char *TAG = "CENTRAL";

// =========================================================
// WIFI + MQTT CONFIG
// =========================================================

#define WIFI_SSID       "example_ssid"
#define WIFI_PASSWORD   "example_password"

// #define MQTT_BROKER_URI  e.g "mqtt://192.168.1.100:1883"
#define MQTT_BROKER_URI "mqtt://192.168.x.x:1883"

// Router / Hotspot 2.4 GHz phai su dung chung channel

// =========================================================
// MQTT TOPICS
// =========================================================

#define MQTT_TOPIC_AI        "smartroom/status/ai"
#define MQTT_TOPIC_ACTUATOR  "smartroom/status/actuator"
#define MQTT_TOPIC_SYSTEM    "smartroom/status/system"

#define MQTT_TOPIC_CONTROL   "smartroom/cmd/control"
#define MQTT_TOPIC_TIME      "smartroom/cmd/time"

// =========================================================
// ROOM STATE
// =========================================================

#define NUM_ROOM_STATES 4

#define ROOM_EMPTY    0
#define ROOM_READING  1
#define ROOM_SLEEP    2
#define ROOM_ACTIVE   3

static const char *kRoomStateLabels[NUM_ROOM_STATES] = {
    "Empty",
    "Reading",
    "Sleep",
    "Active"
};

// =========================================================
// AUTO POLICY
// =========================================================

// Relay policy
#define AUTO_RELAY_EMPTY    0
#define AUTO_RELAY_READING  (RELAY_1_BIT | RELAY_2_BIT)
#define AUTO_RELAY_SLEEP    0
#define AUTO_RELAY_ACTIVE   RELAY_2_BIT

// AC target
#define AUTO_TEMP_EMPTY     27
#define AUTO_TEMP_READING   26
#define AUTO_TEMP_SLEEP     27
#define AUTO_TEMP_ACTIVE    25

#define AC_HYSTERESIS       1.0f

static bool s_auto_enabled = true;

// =========================================================
// ACTUATOR MAC
// =========================================================

// Node 3 STA MAC:

static uint8_t s_actuator_mac[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

// =========================================================
// WIFI / MQTT STATE
// =========================================================

static bool s_wifi_connected = false;
static bool s_mqtt_connected = false;

static esp_mqtt_client_handle_t s_mqtt_client = NULL;

// =========================================================
// CENTRAL STATE
// =========================================================

typedef struct {
    uint8_t room_state;
    float confidence;
    int64_t last_update_ms;
    bool valid;
} central_ai_state_t;

typedef struct {
    float room_temp;
    rtc_datetime_t datetime;

    uint8_t seq;
    uint8_t relay_mask;
    uint8_t load_mask;

    uint8_t ac_power;
    uint8_t target_temp;
    uint8_t control_mode;

    int64_t last_update_ms;
    bool valid;
} central_actuator_state_t;

typedef struct {
    uint8_t relay_mask;
    bool ac_power;
    uint8_t target_temp;
} auto_control_t;

static central_ai_state_t s_ai_state = {
    .room_state = ROOM_EMPTY,
    .confidence = 0.0f,
    .last_update_ms = 0,
    .valid = false
};

static central_actuator_state_t s_actuator_state = {
    .room_temp = 0.0f,
    .datetime = {},
    .seq = 0,

    .relay_mask = 0,
    .load_mask = 0,

    .ac_power = 0,
    .target_temp = 25,

    .control_mode = CONTROL_MODE_AUTO,

    .last_update_ms = 0,
    .valid = false
};

static uint8_t s_command_seq = 0;
static uint8_t s_time_seq = 0;

// =========================================================
// ESP-NOW RX
// =========================================================

#define RX_DATA_MAX_SIZE sizeof(actuator_status_packet_t)

typedef struct {
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
    uint8_t data[RX_DATA_MAX_SIZE];
    int data_len;
} central_rx_event_t;

static QueueHandle_t s_rx_queue = NULL;

// =========================================================
// FUNCTION PROTOTYPES
// =========================================================

static void evaluate_auto_control(void);
static void set_auto_enabled(bool enabled);

static esp_err_t send_actuator_command(
    uint8_t relay_mask,
    bool ac_power,
    uint8_t target_temp,
    uint8_t control_mode);

static esp_err_t send_time_sync(
    const rtc_datetime_t *datetime);

static void mqtt_publish_ai(void);
static void mqtt_publish_actuator(void);
static void mqtt_publish_system(void);

// =========================================================
// UTILITY
// =========================================================

static bool actuator_mac_configured(void)
{
    for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
        if (s_actuator_mac[i] != 0x00) {
            return true;
        }
    }

    return false;
}

// DS3231 convention:
// 1 = Sunday
// 2 = Monday
// ...
// 7 = Saturday

static uint8_t calculate_day_of_week(
    uint16_t year,
    uint8_t month,
    uint8_t day)
{
    static const uint8_t table[] = {
        0, 3, 2, 5, 0, 3,
        5, 1, 4, 6, 2, 4
    };

    uint16_t y = year;

    if (month < 3) {
        y--;
    }

    uint8_t dow =
        (y +
         y / 4 -
         y / 100 +
         y / 400 +
         table[month - 1] +
         day) % 7;

    return dow + 1;
}

// =========================================================
// VERIFY AI PACKET
// =========================================================

static bool verify_ai_packet(
    const room_ai_packet_t *packet)
{
    if (packet == NULL) {
        return false;
    }

    if (packet->magic != ESPNOW_PACKET_MAGIC_AI) {
        ESP_LOGW(
            TAG,
            "AI magic sai: 0x%08lx",
            (unsigned long)packet->magic);

        return false;
    }

    if (packet->node_id != NODE_CAM_AI) {
        ESP_LOGW(
            TAG,
            "AI node_id sai: %u",
            packet->node_id);

        return false;
    }

    if (packet->room_state >= NUM_ROOM_STATES) {
        ESP_LOGW(
            TAG,
            "room_state khong hop le: %u",
            packet->room_state);

        return false;
    }

    room_ai_packet_t temp = *packet;

    uint16_t received_crc = temp.crc;

    temp.crc = 0;

    uint16_t calculated_crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&temp,
            sizeof(temp) - sizeof(temp.crc));

    if (received_crc != calculated_crc) {
        ESP_LOGW(
            TAG,
            "AI CRC FAIL: RX=0x%04X CALC=0x%04X",
            received_crc,
            calculated_crc);

        return false;
    }

    return true;
}

static void print_wifi_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;

    esp_err_t err = esp_wifi_get_channel(&primary, &second);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "CURRENT WIFI CHANNEL = %u",
            primary);
    } else {
        ESP_LOGW(
            TAG,
            "Cannot read WiFi channel: %s",
            esp_err_to_name(err));
    }
}
// =========================================================
// VERIFY ACTUATOR STATUS
// =========================================================

static bool verify_actuator_status(
    const actuator_status_packet_t *packet)
{
    if (packet == NULL) {
        return false;
    }

    if (packet->magic != ESPNOW_PACKET_MAGIC_STATUS) {
        ESP_LOGW(
            TAG,
            "Status magic sai: 0x%08lx",
            (unsigned long)packet->magic);

        return false;
    }

    if (packet->node_id != NODE_ACTUATOR) {
        ESP_LOGW(
            TAG,
            "Status node_id sai: %u",
            packet->node_id);

        return false;
    }

    if (packet->ac_power > 1) {
        ESP_LOGW(
            TAG,
            "ac_power khong hop le: %u",
            packet->ac_power);

        return false;
    }

    if (packet->target_temp < 16 ||
        packet->target_temp > 30) {

        ESP_LOGW(
            TAG,
            "target_temp khong hop le: %u",
            packet->target_temp);

        return false;
    }

    if (packet->control_mode != CONTROL_MODE_AUTO &&
        packet->control_mode != CONTROL_MODE_MANUAL) {

        ESP_LOGW(
            TAG,
            "control_mode khong hop le: %u",
            packet->control_mode);

        return false;
    }

    actuator_status_packet_t temp = *packet;

    uint16_t received_crc = temp.crc;

    temp.crc = 0;

    uint16_t calculated_crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&temp,
            sizeof(temp) - sizeof(temp.crc));

    if (received_crc != calculated_crc) {
        ESP_LOGW(
            TAG,
            "Status CRC FAIL: RX=0x%04X CALC=0x%04X",
            received_crc,
            calculated_crc);

        return false;
    }

    return true;
}

// =========================================================
// AUTO POLICY
// =========================================================

static auto_control_t get_auto_control(
    uint8_t room_state)
{
    auto_control_t control = {};

    switch (room_state) {

        case ROOM_EMPTY:
            control.relay_mask =
                AUTO_RELAY_EMPTY;

            control.ac_power =
                false;

            control.target_temp =
                AUTO_TEMP_EMPTY;

            break;

        case ROOM_READING:
            control.relay_mask =
                AUTO_RELAY_READING;

            control.target_temp =
                AUTO_TEMP_READING;

            break;

        case ROOM_SLEEP:
            control.relay_mask =
                AUTO_RELAY_SLEEP;

            control.target_temp =
                AUTO_TEMP_SLEEP;

            break;

        case ROOM_ACTIVE:
            control.relay_mask =
                AUTO_RELAY_ACTIVE;

            control.target_temp =
                AUTO_TEMP_ACTIVE;

            break;

        default:
            control.relay_mask = 0;
            control.ac_power = false;
            control.target_temp = 26;
            break;
    }

    // Empty -> tat AC bat buoc
    if (room_state == ROOM_EMPTY) {
        control.ac_power = false;
        return control;
    }

    // AC hysteresis
    if (s_actuator_state.valid) {

        float room_temp =
            s_actuator_state.room_temp;

        if (room_temp >=
            control.target_temp +
            AC_HYSTERESIS) {

            control.ac_power = true;
        }
        else if (
            room_temp <=
            control.target_temp -
            AC_HYSTERESIS) {

            control.ac_power = false;
        }
        else {

            // Nam trong hysteresis band:
            // giu trang thai hien tai

            control.ac_power =
                s_actuator_state.ac_power != 0;
        }
    }

    return control;
}

// =========================================================
// AUTO ENABLE / DISABLE
// =========================================================

static void set_auto_enabled(
    bool enabled)
{
    s_auto_enabled = enabled;

    ESP_LOGI(
        TAG,
        "AUTO mode: %s",
        enabled
            ? "ENABLED"
            : "DISABLED");

    if (enabled) {
        evaluate_auto_control();
    }

    mqtt_publish_system();
}

// =========================================================
// AUTO CONTROL
// =========================================================

static void evaluate_auto_control(void)
{
    if (!s_auto_enabled) {
        return;
    }

    if (!s_ai_state.valid) {
        return;
    }

    if (!s_actuator_state.valid) {
        return;
    }

    auto_control_t desired =
        get_auto_control(
            s_ai_state.room_state);

    bool already_ok =
        s_actuator_state.relay_mask ==
            desired.relay_mask &&

        s_actuator_state.ac_power ==
            (desired.ac_power ? 1 : 0) &&

        s_actuator_state.target_temp ==
            desired.target_temp &&

        s_actuator_state.control_mode ==
            CONTROL_MODE_AUTO;

    if (already_ok) {
        return;
    }

    ESP_LOGI(
        TAG,
        "AUTO | State=%s | Relay=0x%02X | AC=%s | Target=%u C | Room=%.2f C",
        kRoomStateLabels[
            s_ai_state.room_state],

        desired.relay_mask,

        desired.ac_power
            ? "ON"
            : "OFF",

        desired.target_temp,

        s_actuator_state.room_temp);

    esp_err_t err =
        send_actuator_command(
            desired.relay_mask,
            desired.ac_power,
            desired.target_temp,
            CONTROL_MODE_AUTO);

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "AUTO command send fail: %s",
            esp_err_to_name(err));
    }
}

// =========================================================
// MQTT PUBLISH - SYSTEM
// =========================================================

static void mqtt_publish_system(void)
{
    if (!s_mqtt_connected ||
        s_mqtt_client == NULL) {

        return;
    }

    char json[192];

    snprintf(
        json,
        sizeof(json),
        "{"
        "\"online\":true,"
        "\"wifi\":%s,"
        "\"mqtt\":%s,"
        "\"auto\":%s,"
        "\"aiValid\":%s,"
        "\"actuatorValid\":%s"
        "}",

        s_wifi_connected
            ? "true"
            : "false",

        s_mqtt_connected
            ? "true"
            : "false",

        s_auto_enabled
            ? "true"
            : "false",

        s_ai_state.valid
            ? "true"
            : "false",

        s_actuator_state.valid
            ? "true"
            : "false");

    esp_mqtt_client_publish(
        s_mqtt_client,
        MQTT_TOPIC_SYSTEM,
        json,
        0,
        1,
        1);
}

// =========================================================
// MQTT PUBLISH - AI
// =========================================================

static void mqtt_publish_ai(void)
{
    if (!s_mqtt_connected ||
        s_mqtt_client == NULL ||
        !s_ai_state.valid) {

        return;
    }

    char json[192];

    snprintf(
        json,
        sizeof(json),
        "{"
        "\"state\":\"%s\","
        "\"stateId\":%u,"
        "\"confidence\":%.3f"
        "}",

        kRoomStateLabels[
            s_ai_state.room_state],

        s_ai_state.room_state,

        s_ai_state.confidence);

    esp_mqtt_client_publish(
        s_mqtt_client,
        MQTT_TOPIC_AI,
        json,
        0,
        0,
        1);
}

// =========================================================
// MQTT PUBLISH - ACTUATOR
// =========================================================

static void mqtt_publish_actuator(void)
{
    if (!s_mqtt_connected ||
        s_mqtt_client == NULL ||
        !s_actuator_state.valid) {

        return;
    }

    char json[384];

    snprintf(
        json,
        sizeof(json),

        "{"
        "\"relayMask\":%u,"
        "\"loadMask\":%u,"
        "\"relay1\":%s,"
        "\"relay2\":%s,"
        "\"load1\":%s,"
        "\"load2\":%s,"
        "\"acPower\":%s,"
        "\"targetTemp\":%u,"
        "\"roomTemp\":%.2f,"
        "\"mode\":\"%s\","
        "\"rtc\":\"%04u-%02u-%02uT%02u:%02u:%02u\""
        "}",

        s_actuator_state.relay_mask,
        s_actuator_state.load_mask,

        (s_actuator_state.relay_mask &
         RELAY_1_BIT)
            ? "true"
            : "false",

        (s_actuator_state.relay_mask &
         RELAY_2_BIT)
            ? "true"
            : "false",

        (s_actuator_state.load_mask &
         LOAD_1_BIT)
            ? "true"
            : "false",

        (s_actuator_state.load_mask &
         LOAD_2_BIT)
            ? "true"
            : "false",

        s_actuator_state.ac_power
            ? "true"
            : "false",

        s_actuator_state.target_temp,

        s_actuator_state.room_temp,

        s_actuator_state.control_mode ==
                CONTROL_MODE_AUTO
            ? "AUTO"
            : "MANUAL",

        s_actuator_state.datetime.year,
        s_actuator_state.datetime.month,
        s_actuator_state.datetime.day,

        s_actuator_state.datetime.hour,
        s_actuator_state.datetime.minute,
        s_actuator_state.datetime.second);

    esp_mqtt_client_publish(
        s_mqtt_client,
        MQTT_TOPIC_ACTUATOR,
        json,
        0,
        0,
        1);
}

// =========================================================
// MQTT CONTROL FROM FLUTTER
// =========================================================
//
// AUTO:
// {"mode":"AUTO"}
//
// MANUAL:
// {
//   "mode":"MANUAL",
//   "relayMask":3,
//   "acPower":true,
//   "targetTemp":25
// }
//
// Cac field manual co the gui tung phan.
// Neu thieu field, Central giu gia tri hien tai.
// =========================================================

static void process_mqtt_control(
    const char *payload,
    int payload_len)
{
    if (payload == NULL ||
        payload_len <= 0) {

        return;
    }

    char *buffer =
        (char *)calloc(
            payload_len + 1,
            1);

    if (!buffer) {
        return;
    }

    memcpy(
        buffer,
        payload,
        payload_len);

    cJSON *root =
        cJSON_Parse(buffer);

    free(buffer);

    if (!root) {

        ESP_LOGW(
            TAG,
            "MQTT control JSON invalid");

        return;
    }

    cJSON *mode =
        cJSON_GetObjectItem(
            root,
            "mode");

    if (!cJSON_IsString(mode) ||
        mode->valuestring == NULL) {

        ESP_LOGW(
            TAG,
            "MQTT control thieu mode");

        cJSON_Delete(root);
        return;
    }

    // =====================================================
    // AUTO
    // =====================================================

    if (strcmp(
            mode->valuestring,
            "AUTO") == 0) {

        ESP_LOGI(
            TAG,
            "MQTT -> AUTO");

        set_auto_enabled(true);

        cJSON_Delete(root);
        return;
    }

    // =====================================================
    // MANUAL
    // =====================================================

    if (strcmp(
            mode->valuestring,
            "MANUAL") != 0) {

        ESP_LOGW(
            TAG,
            "MQTT mode khong hop le");

        cJSON_Delete(root);
        return;
    }

    set_auto_enabled(false);

    uint8_t relay_mask =
        s_actuator_state.valid
            ? s_actuator_state.relay_mask
            : 0;

    bool ac_power =
        s_actuator_state.valid
            ? s_actuator_state.ac_power != 0
            : false;

    uint8_t target_temp =
        s_actuator_state.valid
            ? s_actuator_state.target_temp
            : 25;

    cJSON *relay =
        cJSON_GetObjectItem(
            root,
            "relayMask");

    cJSON *ac =
        cJSON_GetObjectItem(
            root,
            "acPower");

    cJSON *temp =
        cJSON_GetObjectItem(
            root,
            "targetTemp");

    if (cJSON_IsNumber(relay)) {

        relay_mask =
            ((uint8_t)
                 relay->valueint) &
            (RELAY_1_BIT |
             RELAY_2_BIT);
    }

    if (cJSON_IsBool(ac)) {

        ac_power =
            cJSON_IsTrue(ac);
    }

    if (cJSON_IsNumber(temp)) {

        int value =
            temp->valueint;

        if (value >= 16 &&
            value <= 30) {

            target_temp =
                (uint8_t)value;
        }
    }

    ESP_LOGI(
        TAG,
        "MQTT MANUAL | Relay=0x%02X | AC=%s | Temp=%u",
        relay_mask,
        ac_power
            ? "ON"
            : "OFF",
        target_temp);

    esp_err_t err =
        send_actuator_command(
            relay_mask,
            ac_power,
            target_temp,
            CONTROL_MODE_MANUAL);

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "MQTT manual command fail: %s",
            esp_err_to_name(err));
    }

    mqtt_publish_system();

    cJSON_Delete(root);
}

// =========================================================
// MQTT TIME FROM FLUTTER
// =========================================================
//
// Flutter gui:
//
// {
//   "year":2026,
//   "month":9,
//   "day":23,
//   "hour":14,
//   "minute":30,
//   "second":0
// }
//
// Central tu tinh day_of_week.
// =========================================================

static void process_mqtt_time(
    const char *payload,
    int payload_len)
{
    if (payload == NULL ||
        payload_len <= 0) {

        return;
    }

    char *buffer =
        (char *)calloc(
            payload_len + 1,
            1);

    if (!buffer) {
        return;
    }

    memcpy(
        buffer,
        payload,
        payload_len);

    cJSON *root =
        cJSON_Parse(buffer);

    free(buffer);

    if (!root) {

        ESP_LOGW(
            TAG,
            "MQTT time JSON invalid");

        return;
    }

    cJSON *year =
        cJSON_GetObjectItem(
            root,
            "year");

    cJSON *month =
        cJSON_GetObjectItem(
            root,
            "month");

    cJSON *day =
        cJSON_GetObjectItem(
            root,
            "day");

    cJSON *hour =
        cJSON_GetObjectItem(
            root,
            "hour");

    cJSON *minute =
        cJSON_GetObjectItem(
            root,
            "minute");

    cJSON *second =
        cJSON_GetObjectItem(
            root,
            "second");

    if (!cJSON_IsNumber(year) ||
        !cJSON_IsNumber(month) ||
        !cJSON_IsNumber(day) ||
        !cJSON_IsNumber(hour) ||
        !cJSON_IsNumber(minute) ||
        !cJSON_IsNumber(second)) {

        ESP_LOGW(
            TAG,
            "MQTT datetime thieu field");

        cJSON_Delete(root);
        return;
    }

    rtc_datetime_t dt = {};

    dt.year =
        year->valueint;

    dt.month =
        month->valueint;

    dt.day =
        day->valueint;

    dt.hour =
        hour->valueint;

    dt.minute =
        minute->valueint;

    dt.second =
        second->valueint;

    if (dt.year < 2000 ||
        dt.year > 2099 ||
        dt.month < 1 ||
        dt.month > 12 ||
        dt.day < 1 ||
        dt.day > 31 ||
        dt.hour > 23 ||
        dt.minute > 59 ||
        dt.second > 59) {

        ESP_LOGW(
            TAG,
            "MQTT datetime khong hop le");

        cJSON_Delete(root);
        return;
    }

    dt.day_of_week =
        calculate_day_of_week(
            dt.year,
            dt.month,
            dt.day);

    ESP_LOGI(
        TAG,
        "MQTT TIME | %04u-%02u-%02u %02u:%02u:%02u | DOW=%u",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second,
        dt.day_of_week);

    esp_err_t err =
        send_time_sync(&dt);

    if (err != ESP_OK) {

        ESP_LOGW(
            TAG,
            "MQTT time sync fail: %s",
            esp_err_to_name(err));
    }

    cJSON_Delete(root);
}

// =========================================================
// MQTT EVENT
// =========================================================

static void mqtt_event_handler(
    void *handler_args,
    esp_event_base_t base,
    int32_t event_id,
    void *event_data)
{
    esp_mqtt_event_handle_t event =
        (esp_mqtt_event_handle_t)event_data;

    esp_mqtt_event_id_t mqtt_event_id =
        static_cast<esp_mqtt_event_id_t>(event_id);

    switch (mqtt_event_id) {

        case MQTT_EVENT_CONNECTED:
        {
            s_mqtt_connected = true;

            ESP_LOGI(TAG, "MQTT connected");

            esp_mqtt_client_subscribe(
                s_mqtt_client,
                MQTT_TOPIC_CONTROL,
                1);

            esp_mqtt_client_subscribe(
                s_mqtt_client,
                MQTT_TOPIC_TIME,
                1);

            mqtt_publish_system();
            mqtt_publish_ai();
            mqtt_publish_actuator();

            break;
        }

        case MQTT_EVENT_DISCONNECTED:
        {
            s_mqtt_connected = false;

            ESP_LOGW(TAG, "MQTT disconnected");

            break;
        }

        case MQTT_EVENT_DATA:
        {
            if (event->total_data_len != event->data_len) {
                ESP_LOGW(
                    TAG,
                    "MQTT fragmented payload khong duoc ho tro");

                break;
            }

            ESP_LOGI(
                TAG,
                "MQTT RX | Topic=%.*s",
                event->topic_len,
                event->topic);

            if (
                event->topic_len ==
                    (int)strlen(MQTT_TOPIC_CONTROL) &&

                strncmp(
                    event->topic,
                    MQTT_TOPIC_CONTROL,
                    event->topic_len) == 0)
            {
                process_mqtt_control(
                    event->data,
                    event->data_len);
            }
            else if (
                event->topic_len ==
                    (int)strlen(MQTT_TOPIC_TIME) &&

                strncmp(
                    event->topic,
                    MQTT_TOPIC_TIME,
                    event->topic_len) == 0)
            {
                process_mqtt_time(
                    event->data,
                    event->data_len);
            }

            break;
        }

        case MQTT_EVENT_ERROR:
        {
            ESP_LOGW(TAG, "MQTT error");
            break;
        }

        default:
            break;
    }
}

// =========================================================
// MQTT INIT
// =========================================================

static esp_err_t init_mqtt(void)
{
    esp_mqtt_client_config_t
        mqtt_cfg = {};

    mqtt_cfg.broker.address.uri =
        MQTT_BROKER_URI;

    s_mqtt_client =
        esp_mqtt_client_init(
            &mqtt_cfg);

    if (s_mqtt_client == NULL) {

        ESP_LOGE(
            TAG,
            "MQTT client init fail");

        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(
            s_mqtt_client,
            static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
            mqtt_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_mqtt_client_start(
            s_mqtt_client));

    ESP_LOGI(
        TAG,
        "MQTT client started | Broker=%s",
        MQTT_BROKER_URI);

    return ESP_OK;
}

// =========================================================
// PROCESS AI PACKET
// =========================================================

static void process_ai_packet(
    const uint8_t *data)
{
    room_ai_packet_t packet;

    memcpy(
        &packet,
        data,
        sizeof(packet));

    if (!verify_ai_packet(
            &packet)) {

        ESP_LOGW(
            TAG,
            "AI packet bi loai");

        return;
    }

    s_ai_state.room_state =
        packet.room_state;

    s_ai_state.confidence =
        packet.confidence;

    s_ai_state.last_update_ms =
        esp_timer_get_time() /
        1000;

    s_ai_state.valid =
        true;

    ESP_LOGI(
        TAG,
        "AI OK | State: %-7s | Confidence: %5.1f%%",
        kRoomStateLabels[
            s_ai_state.room_state],

        s_ai_state.confidence *
            100.0f);

    // Gui AI state len Flutter
    mqtt_publish_ai();

    mqtt_publish_system();

    // AUTO policy
    evaluate_auto_control();
}

// =========================================================
// PROCESS ACTUATOR STATUS
// =========================================================

static void process_actuator_status(
    const uint8_t *data)
{
    actuator_status_packet_t packet;

    memcpy(
        &packet,
        data,
        sizeof(packet));

    if (!verify_actuator_status(
            &packet)) {

        ESP_LOGW(
            TAG,
            "Actuator status bi loai");

        return;
    }

    s_actuator_state.room_temp =
        packet.room_temp;

    s_actuator_state.datetime =
        packet.datetime;

    s_actuator_state.seq =
        packet.seq;

    s_actuator_state.relay_mask =
        packet.relay_mask;

    s_actuator_state.load_mask =
        packet.load_mask;

    s_actuator_state.ac_power =
        packet.ac_power;

    s_actuator_state.target_temp =
        packet.target_temp;

    s_actuator_state.control_mode =
        packet.control_mode;

    s_actuator_state.last_update_ms =
        esp_timer_get_time() /
        1000;

    s_actuator_state.valid =
        true;

    ESP_LOGI(
        TAG,
        "ACTUATOR OK | RTC=%04u-%02u-%02u %02u:%02u:%02u | seq=%u | Relay=0x%02X | Load=0x%02X | AC=%s | Set=%u C | Room=%.2f C | Mode=%s",

        packet.datetime.year,
        packet.datetime.month,
        packet.datetime.day,

        packet.datetime.hour,
        packet.datetime.minute,
        packet.datetime.second,

        packet.seq,

        packet.relay_mask,
        packet.load_mask,

        packet.ac_power
            ? "ON"
            : "OFF",

        packet.target_temp,

        packet.room_temp,

        packet.control_mode ==
                CONTROL_MODE_AUTO
            ? "AUTO"
            : "MANUAL");

    // =====================================================
    // LOCAL MANUAL OVERRIDE
    // =====================================================

    // Neu nguoi dung bam nut tren Node 3
    // thi Node 3 chuyen MANUAL.
    // Central dung AUTO cho den khi Flutter bat lai AUTO.

    if (
        packet.control_mode ==
            CONTROL_MODE_MANUAL &&
        s_auto_enabled) {

        s_auto_enabled =
            false;

        ESP_LOGI(
            TAG,
            "AUTO disabled - local MANUAL override");
    }

    // =====================================================
    // LOAD FEEDBACK
    // =====================================================

    uint8_t expected_load =
        packet.relay_mask &
        (RELAY_1_BIT |
         RELAY_2_BIT);

    uint8_t actual_load =
        packet.load_mask &
        (LOAD_1_BIT |
         LOAD_2_BIT);

    if (expected_load !=
        actual_load) {

        ESP_LOGW(
            TAG,
            "LOAD MISMATCH | Expected=0x%02X | Detected=0x%02X",
            expected_load,
            actual_load);
    }

    // Publish cho Flutter
    mqtt_publish_actuator();
    mqtt_publish_system();

    // Tiep tuc AUTO neu dang AUTO
    evaluate_auto_control();
}

// =========================================================
// ESP-NOW RECEIVE CALLBACK
// =========================================================

static void espnow_recv_cb(
    const esp_now_recv_info_t *recv_info,
    const uint8_t *data,
    int data_len)
{
    if (recv_info == NULL ||
        data == NULL) {

        return;
    }

    if (data_len <= 0 ||
        data_len >
            RX_DATA_MAX_SIZE) {

        ESP_LOGW(
            TAG,
            "RX packet size khong hop le: %d",
            data_len);

        return;
    }

    central_rx_event_t evt = {};

    memcpy(
        evt.src_mac,
        recv_info->src_addr,
        ESP_NOW_ETH_ALEN);

    memcpy(
        evt.data,
        data,
        data_len);

    evt.data_len =
        data_len;

    if (xQueueSend(
            s_rx_queue,
            &evt,
            0) != pdTRUE) {

        ESP_LOGW(
            TAG,
            "RX queue day, bo packet");
    }
}

// =========================================================
// ESP-NOW SEND CALLBACK
// =========================================================

static void espnow_send_cb(
    const esp_now_send_info_t *tx_info,
    esp_now_send_status_t status)
{
    if (tx_info == NULL) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Packet gui toi " MACSTR ", status: %s",

        MAC2STR(
            tx_info->des_addr),

        status ==
                ESP_NOW_SEND_SUCCESS
            ? "SUCCESS"
            : "FAIL");
}

// =========================================================
// ESP-NOW RX TASK
// =========================================================

static void espnow_rx_task(
    void *pvParameter)
{
    central_rx_event_t evt;

    while (
        xQueueReceive(
            s_rx_queue,
            &evt,
            portMAX_DELAY) ==
        pdTRUE) {

        if (evt.data_len <
            sizeof(uint32_t)) {

            ESP_LOGW(
                TAG,
                "Packet qua ngan");

            continue;
        }

        uint32_t magic;

        memcpy(
            &magic,
            evt.data,
            sizeof(magic));

        // =================================================
        // CAMERA -> CENTRAL
        // =================================================

        if (
            magic ==
                ESPNOW_PACKET_MAGIC_AI &&

            evt.data_len ==
                sizeof(
                    room_ai_packet_t)) {

            ESP_LOGI(
                TAG,
                "RX AI tu " MACSTR,
                MAC2STR(evt.src_mac));

            process_ai_packet(
                evt.data);
        }

        // =================================================
        // ACTUATOR -> CENTRAL
        // =================================================

        else if (
            magic ==
                ESPNOW_PACKET_MAGIC_STATUS &&

            evt.data_len ==
                sizeof(
                    actuator_status_packet_t)) {

            ESP_LOGI(
                TAG,
                "RX STATUS tu " MACSTR,
                MAC2STR(evt.src_mac));

            process_actuator_status(
                evt.data);
        }

        else {

            ESP_LOGW(
                TAG,
                "Packet khong xac dinh | MAC=" MACSTR " | Magic=0x%08lx | Len=%d",

                MAC2STR(evt.src_mac),

                (unsigned long)magic,

                evt.data_len);
        }
    }
}

// =========================================================
// SEND COMMAND -> ACTUATOR
// =========================================================

static esp_err_t send_actuator_command(
    uint8_t relay_mask,
    bool ac_power,
    uint8_t target_temp,
    uint8_t control_mode)
{
    if (!actuator_mac_configured()) {

        ESP_LOGW(
            TAG,
            "Chua cau hinh MAC cua Actuator");

        return ESP_ERR_INVALID_STATE;
    }

    if (target_temp < 16 ||
        target_temp > 30) {

        ESP_LOGE(
            TAG,
            "Target temperature khong hop le: %u",
            target_temp);

        return ESP_ERR_INVALID_ARG;
    }

    if (
        control_mode !=
            CONTROL_MODE_AUTO &&

        control_mode !=
            CONTROL_MODE_MANUAL) {

        ESP_LOGE(
            TAG,
            "Control mode khong hop le");

        return ESP_ERR_INVALID_ARG;
    }

    actuator_command_packet_t packet = {};

    packet.magic =
        ESPNOW_PACKET_MAGIC_CMD;

    packet.node_id =
        NODE_CENTRAL;

    packet.seq =
        ++s_command_seq;

    packet.relay_mask =
        relay_mask &
        (RELAY_1_BIT |
         RELAY_2_BIT);

    packet.ac_power =
        ac_power ? 1 : 0;

    packet.target_temp =
        target_temp;

    packet.control_mode =
        control_mode;

    packet.crc = 0;

    packet.crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&packet,
            sizeof(packet) -
                sizeof(packet.crc));

    ESP_LOGI(
        TAG,
        "TX CMD | seq=%u | Relay=0x%02X | AC=%s | Temp=%u | Mode=%s",

        packet.seq,

        packet.relay_mask,

        packet.ac_power
            ? "ON"
            : "OFF",

        packet.target_temp,

        packet.control_mode ==
                CONTROL_MODE_AUTO
            ? "AUTO"
            : "MANUAL");

    return esp_now_send(
        s_actuator_mac,
        (uint8_t *)&packet,
        sizeof(packet));
}

// =========================================================
// TIME SYNC -> ACTUATOR
// =========================================================

static esp_err_t send_time_sync(
    const rtc_datetime_t *datetime)
{
    if (datetime == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!actuator_mac_configured()) {

        ESP_LOGW(
            TAG,
            "Chua cau hinh MAC cua Actuator");

        return ESP_ERR_INVALID_STATE;
    }

    time_sync_packet_t packet = {};

    packet.magic =
        ESPNOW_PACKET_MAGIC_TIME;

    packet.datetime =
        *datetime;

    packet.node_id =
        NODE_CENTRAL;

    packet.seq =
        ++s_time_seq;

    packet.crc = 0;

    packet.crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&packet,
            sizeof(packet) -
                sizeof(packet.crc));

    ESP_LOGI(
        TAG,
        "TIME SYNC TX | %04u-%02u-%02u %02u:%02u:%02u | DOW=%u",

        datetime->year,
        datetime->month,
        datetime->day,

        datetime->hour,
        datetime->minute,
        datetime->second,

        datetime->day_of_week);

    return esp_now_send(
        s_actuator_mac,
        (uint8_t *)&packet,
        sizeof(packet));
}

// =========================================================
// ADD ACTUATOR PEER
// =========================================================

static esp_err_t add_actuator_peer(void)
{
    if (!actuator_mac_configured()) {

        ESP_LOGW(
            TAG,
            "Actuator MAC chua duoc cau hinh");

        return ESP_OK;
    }

    if (esp_now_is_peer_exist(
            s_actuator_mac)) {

        return ESP_OK;
    }

    esp_now_peer_info_t peer = {};

    memcpy(
        peer.peer_addr,
        s_actuator_mac,
        ESP_NOW_ETH_ALEN);

    // 0 = current Wi-Fi channel.
    // Central dang ket noi router channel 1.

    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t err =
        esp_now_add_peer(
            &peer);

    if (err == ESP_OK) {

        ESP_LOGI(
            TAG,
            "Da them Actuator peer: "
            MACSTR,

            MAC2STR(
                s_actuator_mac));
    }

    return err;
}

// =========================================================
// WIFI EVENT
// =========================================================

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    // =====================================================
    // START
    // =====================================================

    if (
        event_base ==
            WIFI_EVENT &&

        event_id ==
            WIFI_EVENT_STA_START) {

        ESP_LOGI(
            TAG,
            "WiFi started -> connecting");

        esp_wifi_connect();
    }

    // =====================================================
    // DISCONNECTED
    // =====================================================

    else if (
        event_base ==
            WIFI_EVENT &&

        event_id ==
            WIFI_EVENT_STA_DISCONNECTED) {

        s_wifi_connected =
            false;

        ESP_LOGW(
            TAG,
            "WiFi disconnected -> reconnecting");

        esp_wifi_connect();
    }

    // =====================================================
    // GOT IP
    // =====================================================

    else if (
        event_base ==
            IP_EVENT &&

        event_id ==
            IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)
                event_data;

        s_wifi_connected =
            true;

        ESP_LOGI(
            TAG,
            "WiFi connected | IP=" IPSTR,

            IP2STR(
                &event->ip_info.ip));
        print_wifi_channel();
        mqtt_publish_system();
    }
}

// =========================================================
// WIFI + ESP-NOW INIT
// =========================================================

static esp_err_t init_wifi_espnow(void)
{
    s_rx_queue =
        xQueueCreate(
            ESPNOW_QUEUE_SIZE,
            sizeof(
                central_rx_event_t));

    if (!s_rx_queue) {

        ESP_LOGE(
            TAG,
            "Khong tao duoc RX queue");

        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default());

    // Tao default Wi-Fi STA network interface
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(
            &cfg));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL));

    ESP_ERROR_CHECK(
        esp_wifi_set_storage(
            WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA));

    // =====================================================
    // WIFI CONFIG
    // =====================================================

    wifi_config_t wifi_config = {};

    strncpy(
        (char *)
            wifi_config.sta.ssid,
        WIFI_SSID,
        sizeof(
            wifi_config.sta.ssid) -
            1);

    strncpy(
        (char *)
            wifi_config.sta.password,
        WIFI_PASSWORD,
        sizeof(
            wifi_config.sta.password) -
            1);

    // Chi scan/connect AP tren channel 1.
    // Dam bao ESP-NOW va Wi-Fi cung channel.

    wifi_config.sta.channel =
        ESPNOW_WIFI_CHANNEL;

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    // ESP-NOW realtime -> tat power saving
    ESP_ERROR_CHECK(
        esp_wifi_set_ps(
            WIFI_PS_NONE));

    // =====================================================
    // ESP-NOW INIT
    // =====================================================

    ESP_ERROR_CHECK(
        esp_now_init());

    ESP_ERROR_CHECK(
        esp_now_register_recv_cb(
            espnow_recv_cb));

    ESP_ERROR_CHECK(
        esp_now_register_send_cb(
            espnow_send_cb));

    ESP_ERROR_CHECK(
        add_actuator_peer());

    xTaskCreate(
        espnow_rx_task,
        "espnow_rx_task",
        4096,
        NULL,
        5,
        NULL);

    uint8_t mac[
        ESP_NOW_ETH_ALEN];

    ESP_ERROR_CHECK(
        esp_wifi_get_mac(
            WIFI_IF_STA,
            mac));

    ESP_LOGI(
        TAG,
        "Central STA MAC: "
        MACSTR,

        MAC2STR(mac));

    ESP_LOGI(
        TAG,
        "ESP-NOW target channel: %u",
        ESPNOW_WIFI_CHANNEL);

    return ESP_OK;
}

// =========================================================
// APP MAIN
// =========================================================

extern "C" void app_main(void)
{
    // =====================================================
    // NVS
    // =====================================================

    esp_err_t ret =
        nvs_flash_init();

    if (
        ret ==
            ESP_ERR_NVS_NO_FREE_PAGES ||

        ret ==
            ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ret =
            nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // =====================================================
    // WIFI + ESP-NOW
    // =====================================================

    ESP_ERROR_CHECK(
        init_wifi_espnow());

    // =====================================================
    // MQTT
    // =====================================================

    ESP_ERROR_CHECK(
        init_mqtt());

    // =====================================================
    // DEFAULT AUTO
    // =====================================================

    s_auto_enabled = true;

    ESP_LOGI(
        TAG,
        "Central Node ready | AUTO=ON");

    ESP_LOGI(
        TAG,
        "MQTT topics:");

    ESP_LOGI(
        TAG,
        "PUB %s",
        MQTT_TOPIC_AI);

    ESP_LOGI(
        TAG,
        "PUB %s",
        MQTT_TOPIC_ACTUATOR);

    ESP_LOGI(
        TAG,
        "PUB %s",
        MQTT_TOPIC_SYSTEM);

    ESP_LOGI(
        TAG,
        "SUB %s",
        MQTT_TOPIC_CONTROL);

    ESP_LOGI(
        TAG,
        "SUB %s",
        MQTT_TOPIC_TIME);
}