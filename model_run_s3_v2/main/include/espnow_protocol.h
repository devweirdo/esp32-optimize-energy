#pragma once

#include <stdint.h>
#include "esp_now.h"

// =========================================================
// COMMON CONFIG
// =========================================================

#define ESPNOW_QUEUE_SIZE           6
#define ESPNOW_WIFI_CHANNEL         6

#define ESPNOW_PACKET_MAGIC_AI      0x53334149
#define ESPNOW_PACKET_MAGIC_CMD     0x41435443
#define ESPNOW_PACKET_MAGIC_STATUS  0x41435453
#define ESPNOW_PACKET_MAGIC_TIME    0x54494D45

// Relay command bit mask
#define RELAY_1_BIT                 (1 << 0)
#define RELAY_2_BIT                 (1 << 1)

// Detected load bit mask
#define LOAD_1_BIT                  (1 << 0)
#define LOAD_2_BIT                  (1 << 1)

// =========================================================
// NODE ID
// =========================================================

typedef enum {
    NODE_CENTRAL  = 1,
    NODE_CAM_AI   = 2,
    NODE_ACTUATOR = 3
} node_id_t;

// =========================================================
// CONTROL MODE
// =========================================================

typedef enum {
    CONTROL_MODE_AUTO   = 0,
    CONTROL_MODE_MANUAL = 1
} control_mode_t;

// =========================================================
// ESP-NOW EVENT
// =========================================================

typedef enum {
    ESPNOW_SEND_CB,
    ESPNOW_RECV_CB
} espnow_event_id_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} espnow_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} espnow_event_recv_cb_t;

typedef struct {
    espnow_event_id_t id;

    union {
        espnow_event_send_cb_t send_cb;
        espnow_event_recv_cb_t recv_cb;
    } info;
} espnow_event_t;

// =========================================================
// CAMERA -> CENTRAL
// =========================================================

typedef struct __attribute__((packed)) {
    uint32_t magic;
    float confidence;
    uint8_t node_id;
    uint8_t room_state;
    uint16_t crc;
} room_ai_packet_t;

// =========================================================
// CENTRAL -> ACTUATOR
// =========================================================

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t node_id;
    uint8_t seq;
    uint8_t relay_mask;
    uint8_t ac_power;
    uint8_t target_temp;
    uint8_t control_mode;
    uint16_t crc;
} actuator_command_packet_t;

// =========================================================
// RTC DATETIME
// =========================================================

typedef struct __attribute__((packed)) {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t day_of_week;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_datetime_t;

// =========================================================
// ACTUATOR -> CENTRAL
// =========================================================

typedef struct __attribute__((packed)) {
    uint32_t magic;
    float room_temp;
    rtc_datetime_t datetime;

    uint8_t node_id;
    uint8_t seq;
    uint8_t relay_mask;
    uint8_t load_mask;
    uint8_t ac_power;
    uint8_t target_temp;
    uint8_t control_mode;

    uint16_t crc;
} actuator_status_packet_t;

// =========================================================
// CENTRAL -> ACTUATOR : RTC SYNC
// =========================================================

typedef struct __attribute__((packed)) {
    uint32_t magic;
    rtc_datetime_t datetime;
    uint8_t node_id;
    uint8_t seq;
    uint16_t crc;
} time_sync_packet_t;

// =========================================================
// PACKET SIZE CHECK
// =========================================================

#ifdef __cplusplus

static_assert(sizeof(room_ai_packet_t) == 12,
              "room_ai_packet_t must be 12 bytes");

static_assert(sizeof(actuator_command_packet_t) == 12,
              "actuator_command_packet_t must be 12 bytes");

static_assert(sizeof(rtc_datetime_t) == 8,
              "rtc_datetime_t must be 8 bytes");

static_assert(sizeof(time_sync_packet_t) == 16,
              "time_sync_packet_t must be 16 bytes");

static_assert(sizeof(actuator_status_packet_t) == 25,
              "actuator_status_packet_t must be 25 bytes");

#endif