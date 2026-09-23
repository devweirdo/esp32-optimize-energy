#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_crc.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "espnow_protocol.h"
#include "ir_transmitter.h"

static const char *TAG = "ACTUATOR";

// =========================================================
// BUTTONS
// =========================================================

#define BTN_TEMP_UP_PIN      GPIO_NUM_0
#define BTN_TEMP_DOWN_PIN    GPIO_NUM_1
#define BTN_AC_TOGGLE_PIN    GPIO_NUM_10

// =========================================================
// I2C
// =========================================================

#define I2C_PORT        I2C_NUM_0
#define I2C_SDA_PIN     GPIO_NUM_6
#define I2C_SCL_PIN     GPIO_NUM_7

#define DS3231_ADDR     0x68
#define SSD1306_ADDR    0x3C

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_rtc_dev = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;

// =========================================================
// RELAY
// =========================================================

#define RELAY_1_PIN GPIO_NUM_18
#define RELAY_2_PIN GPIO_NUM_19

// Module relay dang su dung active LOW.
#define RELAY_ACTIVE_LEVEL 0

// =========================================================
// CURRENT SENSOR
// =========================================================

#define CT1_ADC_CHANNEL ADC_CHANNEL_2
#define CT2_ADC_CHANNEL ADC_CHANNEL_3

#define CT_SAMPLE_WINDOW_MS 120
#define CT_ADC_MAX          4095

#define CT1_LOAD_THRESHOLD 350.0f
#define CT2_LOAD_THRESHOLD 430.0f

static adc_oneshot_unit_handle_t
    s_adc_handle = NULL;

static volatile int
    s_ct1_p2p_raw = 0;

static volatile int
    s_ct2_p2p_raw = 0;

static volatile float
    s_ct1_p2p_filtered = 0.0f;

static volatile float
    s_ct2_p2p_filtered = 0.0f;

static volatile bool
    s_ct1_load_detected = false;

static volatile bool
    s_ct2_load_detected = false;

// =========================================================
// CENTRAL MAC
// =========================================================

static uint8_t
    s_central_mac[ESP_NOW_ETH_ALEN] = {
        0xff, 0xff, 0xff,
        0xff, 0xff, 0xff
    };

// =========================================================
// NODE STATUS
// =========================================================

typedef struct {
    int target_temp;
    bool is_power_on;
    float room_temp;

    rtc_datetime_t datetime;

    uint8_t relay_mask;
    uint8_t control_mode;
    uint8_t last_seq;
} node3_status_t;

static node3_status_t
    s_status = {};

static SemaphoreHandle_t
    s_status_mutex = NULL;

static SemaphoreHandle_t
    s_ir_mutex = NULL;

// =========================================================
// ESP-NOW RX
// =========================================================

#define RX_DATA_MAX_SIZE 32

typedef struct {
    uint8_t src_mac[ESP_NOW_ETH_ALEN];
    uint8_t data[RX_DATA_MAX_SIZE];
    int data_len;
} actuator_rx_event_t;

static QueueHandle_t
    s_rx_queue = NULL;

// =========================================================
// PROTOTYPES
// =========================================================

static void init_relays(void);
static void set_relays(uint8_t relay_mask);

static esp_err_t
    init_current_sensors(void);

static void
    current_sensor_task(
        void *pvParameter);

// =========================================================
// OLED
// =========================================================

static uint8_t
    s_oled_buffer[128 * 8];

// =========================================================
// UTILITY
// =========================================================

static uint8_t bcd_to_dec(
    uint8_t value)
{
    return ((value >> 4) * 10) +
           (value & 0x0F);
}

static uint8_t dec_to_bcd(
    uint8_t value)
{
    return ((value / 10) << 4) |
           (value % 10);
}

static bool valid_datetime(
    const rtc_datetime_t *dt)
{
    if (dt == NULL)
        return false;

    if (dt->year < 2000 ||
        dt->year > 2099)
        return false;

    if (dt->month < 1 ||
        dt->month > 12)
        return false;

    if (dt->day < 1 ||
        dt->day > 31)
        return false;

    if (dt->day_of_week < 1 ||
        dt->day_of_week > 7)
        return false;

    if (dt->hour > 23 ||
        dt->minute > 59 ||
        dt->second > 59)
        return false;

    return true;
}

// =========================================================
// I2C INIT
// =========================================================

static esp_err_t init_i2c_bus(void)
{
    i2c_master_bus_config_t
        bus_config = {};

    bus_config.i2c_port =
        I2C_PORT;

    bus_config.sda_io_num =
        I2C_SDA_PIN;

    bus_config.scl_io_num =
        I2C_SCL_PIN;

    bus_config.clk_source =
        I2C_CLK_SRC_DEFAULT;

    bus_config.glitch_ignore_cnt =
        7;

    bus_config.flags.enable_internal_pullup =
        true;

    ESP_ERROR_CHECK(
        i2c_new_master_bus(
            &bus_config,
            &s_i2c_bus));

    i2c_device_config_t
        rtc_config = {};

    rtc_config.dev_addr_length =
        I2C_ADDR_BIT_LEN_7;

    rtc_config.device_address =
        DS3231_ADDR;

    rtc_config.scl_speed_hz =
        100000;

    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            s_i2c_bus,
            &rtc_config,
            &s_rtc_dev));

    i2c_device_config_t
        oled_config = {};

    oled_config.dev_addr_length =
        I2C_ADDR_BIT_LEN_7;

    oled_config.device_address =
        SSD1306_ADDR;

    oled_config.scl_speed_hz =
        400000;

    ESP_ERROR_CHECK(
        i2c_master_bus_add_device(
            s_i2c_bus,
            &oled_config,
            &s_oled_dev));

    ESP_LOGI(
        TAG,
        "I2C initialized");

    return ESP_OK;
}

// =========================================================
// DS3231
// =========================================================

static esp_err_t ds3231_read_datetime(
    rtc_datetime_t *dt,
    float *temperature)
{
    if (dt == NULL ||
        temperature == NULL)
        return ESP_ERR_INVALID_ARG;

    uint8_t reg = 0x00;
    uint8_t data[7];

    esp_err_t err =
        i2c_master_transmit_receive(
            s_rtc_dev,
            &reg,
            1,
            data,
            7,
            1000);

    if (err != ESP_OK)
        return err;

    dt->second =
        bcd_to_dec(data[0] & 0x7F);

    dt->minute =
        bcd_to_dec(data[1] & 0x7F);

    if (data[2] & 0x40) {

        uint8_t h =
            bcd_to_dec(
                data[2] & 0x1F);

        bool pm =
            (data[2] & 0x20) != 0;

        if (h == 12)
            h = 0;

        dt->hour =
            h + (pm ? 12 : 0);
    }
    else {
        dt->hour =
            bcd_to_dec(
                data[2] & 0x3F);
    }

    dt->day_of_week =
        bcd_to_dec(
            data[3] & 0x07);

    dt->day =
        bcd_to_dec(
            data[4] & 0x3F);

    dt->month =
        bcd_to_dec(
            data[5] & 0x1F);

    dt->year =
        2000 +
        bcd_to_dec(data[6]);

    reg = 0x11;

    uint8_t temp_data[2];

    err =
        i2c_master_transmit_receive(
            s_rtc_dev,
            &reg,
            1,
            temp_data,
            2,
            1000);

    if (err != ESP_OK)
        return err;

    *temperature =
        (int8_t)temp_data[0] +
        ((temp_data[1] >> 6) *
         0.25f);

    return ESP_OK;
}

static esp_err_t ds3231_set_datetime(
    const rtc_datetime_t *dt)
{
    if (!valid_datetime(dt))
        return ESP_ERR_INVALID_ARG;

    uint8_t data[8];

    data[0] = 0x00;
    data[1] = dec_to_bcd(dt->second);
    data[2] = dec_to_bcd(dt->minute);
    data[3] = dec_to_bcd(dt->hour);
    data[4] = dec_to_bcd(dt->day_of_week);
    data[5] = dec_to_bcd(dt->day);
    data[6] = dec_to_bcd(dt->month);
    data[7] = dec_to_bcd(dt->year - 2000);

    return i2c_master_transmit(
        s_rtc_dev,
        data,
        sizeof(data),
        1000);
}

// =========================================================
// SSD1306
// =========================================================

static esp_err_t oled_command(
    uint8_t command)
{
    uint8_t data[2] = {
        0x00,
        command
    };

    return i2c_master_transmit(
        s_oled_dev,
        data,
        sizeof(data),
        1000);
}

static esp_err_t oled_init(void)
{
    const uint8_t commands[] = {
        0xAE, 0x20, 0x00, 0xB0,
        0xC8, 0x00, 0x10, 0x40,
        0x81, 0x7F, 0xA1, 0xA6,
        0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9,
        0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };

    for (size_t i = 0;
         i < sizeof(commands);
         i++) {

        esp_err_t err =
            oled_command(commands[i]);

        if (err != ESP_OK)
            return err;
    }

    memset(
        s_oled_buffer,
        0,
        sizeof(s_oled_buffer));

    return ESP_OK;
}

static void oled_clear(void)
{
    memset(
        s_oled_buffer,
        0,
        sizeof(s_oled_buffer));
}

static void get_char_bitmap(
    char c,
    uint8_t out[5])
{
    memset(out, 0, 5);

    switch (c) {

        case '0': {
            uint8_t a[5] =
                {0x3E,0x51,0x49,0x45,0x3E};
            memcpy(out,a,5);
            break;
        }

        case '1': {
            uint8_t a[5] =
                {0x00,0x42,0x7F,0x40,0x00};
            memcpy(out,a,5);
            break;
        }

        case '2': {
            uint8_t a[5] =
                {0x42,0x61,0x51,0x49,0x46};
            memcpy(out,a,5);
            break;
        }

        case '3': {
            uint8_t a[5] =
                {0x21,0x41,0x45,0x4B,0x31};
            memcpy(out,a,5);
            break;
        }

        case '4': {
            uint8_t a[5] =
                {0x18,0x14,0x12,0x7F,0x10};
            memcpy(out,a,5);
            break;
        }

        case '5': {
            uint8_t a[5] =
                {0x27,0x45,0x45,0x45,0x39};
            memcpy(out,a,5);
            break;
        }

        case '6': {
            uint8_t a[5] =
                {0x3C,0x4A,0x49,0x49,0x30};
            memcpy(out,a,5);
            break;
        }

        case '7': {
            uint8_t a[5] =
                {0x01,0x71,0x09,0x05,0x03};
            memcpy(out,a,5);
            break;
        }

        case '8': {
            uint8_t a[5] =
                {0x36,0x49,0x49,0x49,0x36};
            memcpy(out,a,5);
            break;
        }

        case '9': {
            uint8_t a[5] =
                {0x06,0x49,0x49,0x29,0x1E};
            memcpy(out,a,5);
            break;
        }

        case 'A': {
            uint8_t a[5] =
                {0x7E,0x11,0x11,0x11,0x7E};
            memcpy(out,a,5);
            break;
        }

        case 'C': {
            uint8_t a[5] =
                {0x3E,0x41,0x41,0x41,0x22};
            memcpy(out,a,5);
            break;
        }

        case 'D': {
            uint8_t a[5] =
                {0x7F,0x41,0x41,0x22,0x1C};
            memcpy(out,a,5);
            break;
        }

        case 'E': {
            uint8_t a[5] =
                {0x7F,0x49,0x49,0x49,0x41};
            memcpy(out,a,5);
            break;
        }

        case 'F': {
            uint8_t a[5] =
                {0x7F,0x09,0x09,0x09,0x01};
            memcpy(out,a,5);
            break;
        }

        case 'I': {
            uint8_t a[5] =
                {0x00,0x41,0x7F,0x41,0x00};
            memcpy(out,a,5);
            break;
        }

        case 'M': {
            uint8_t a[5] =
                {0x7F,0x02,0x0C,0x02,0x7F};
            memcpy(out,a,5);
            break;
        }

        case 'N': {
            uint8_t a[5] =
                {0x7F,0x04,0x08,0x10,0x7F};
            memcpy(out,a,5);
            break;
        }

        case 'O': {
            uint8_t a[5] =
                {0x3E,0x41,0x41,0x41,0x3E};
            memcpy(out,a,5);
            break;
        }

        case 'P': {
            uint8_t a[5] =
                {0x7F,0x09,0x09,0x09,0x06};
            memcpy(out,a,5);
            break;
        }

        case 'R': {
            uint8_t a[5] =
                {0x7F,0x09,0x19,0x29,0x46};
            memcpy(out,a,5);
            break;
        }

        case 'S': {
            uint8_t a[5] =
                {0x46,0x49,0x49,0x49,0x31};
            memcpy(out,a,5);
            break;
        }

        case 'T': {
            uint8_t a[5] =
                {0x01,0x01,0x7F,0x01,0x01};
            memcpy(out,a,5);
            break;
        }

        case 'U': {
            uint8_t a[5] =
                {0x3F,0x40,0x40,0x40,0x3F};
            memcpy(out,a,5);
            break;
        }

        case ':': {
            uint8_t a[5] =
                {0x00,0x36,0x36,0x00,0x00};
            memcpy(out,a,5);
            break;
        }

        case '.': {
            uint8_t a[5] =
                {0x00,0x60,0x60,0x00,0x00};
            memcpy(out,a,5);
            break;
        }

        case '-': {
            uint8_t a[5] =
                {0x08,0x08,0x08,0x08,0x08};
            memcpy(out,a,5);
            break;
        }

        default:
            break;
    }
}

static void oled_draw_char(
    int x,
    int page,
    char c)
{
    if (x < 0 ||
        x + 5 >= 128 ||
        page < 0 ||
        page >= 8)
        return;

    uint8_t bitmap[5];

    get_char_bitmap(
        c,
        bitmap);

    for (int i = 0; i < 5; i++) {
        s_oled_buffer[
            page * 128 +
            x + i] =
            bitmap[i];
    }

    s_oled_buffer[
        page * 128 +
        x + 5] =
        0x00;
}

static void oled_draw_string(
    int x,
    int page,
    const char *text)
{
    while (*text &&
           x < 122) {

        oled_draw_char(
            x,
            page,
            *text++);

        x += 6;
    }
}

static esp_err_t oled_render(void)
{
    uint8_t packet[129];

    packet[0] = 0x40;

    for (int page = 0;
         page < 8;
         page++) {

        esp_err_t err =
            oled_command(
                0xB0 + page);

        if (err != ESP_OK)
            return err;

        err =
            oled_command(0x00);

        if (err != ESP_OK)
            return err;

        err =
            oled_command(0x10);

        if (err != ESP_OK)
            return err;

        memcpy(
            &packet[1],
            &s_oled_buffer[
                page * 128],
            128);

        err =
            i2c_master_transmit(
                s_oled_dev,
                packet,
                sizeof(packet),
                1000);

        if (err != ESP_OK)
            return err;
    }

    return ESP_OK;
}

// =========================================================
// IR
// =========================================================

static void send_ir_command(
    int target_temp,
    bool power_on)
{
    ir_ac_data_t ac_data;

    if (xSemaphoreTake(
            s_ir_mutex,
            pdMS_TO_TICKS(1000))
        == pdTRUE) {

        set_ac_state(
            &ac_data,
            target_temp,
            power_on);

        ir_ac_transmit(
            &ac_data);

        xSemaphoreGive(
            s_ir_mutex);
    }
}

// =========================================================
// PACKET VALIDATION
// =========================================================

static bool verify_command_packet(
    const actuator_command_packet_t *packet)
{
    if (packet == NULL)
        return false;

    if (packet->magic !=
        ESPNOW_PACKET_MAGIC_CMD) {

        ESP_LOGW(
            TAG,
            "Sai command magic: 0x%08lx",
            (unsigned long)packet->magic);

        return false;
    }

    if (packet->node_id !=
        NODE_CENTRAL) {

        ESP_LOGW(
            TAG,
            "Sai command node_id: %u",
            packet->node_id);

        return false;
    }

    if (packet->ac_power > 1 ||
        packet->target_temp < 16 ||
        packet->target_temp > 30) {

        ESP_LOGW(
            TAG,
            "Command co tham so khong hop le");

        return false;
    }

    if (packet->control_mode !=
            CONTROL_MODE_AUTO &&
        packet->control_mode !=
            CONTROL_MODE_MANUAL) {

        ESP_LOGW(
            TAG,
            "Control mode khong hop le: %u",
            packet->control_mode);

        return false;
    }

    actuator_command_packet_t
        temp = *packet;

    uint16_t received_crc =
        temp.crc;

    temp.crc = 0;

    uint16_t calculated_crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&temp,
            sizeof(temp) -
                sizeof(temp.crc));

    if (received_crc !=
        calculated_crc) {

        ESP_LOGW(
            TAG,
            "Command CRC FAIL: RX=0x%04X CALC=0x%04X",
            received_crc,
            calculated_crc);

        return false;
    }

    return true;
}

static bool verify_time_sync_packet(
    const time_sync_packet_t *packet)
{
    if (packet == NULL)
        return false;

    if (packet->magic !=
        ESPNOW_PACKET_MAGIC_TIME) {

        ESP_LOGW(
            TAG,
            "Sai time magic: 0x%08lx",
            (unsigned long)packet->magic);

        return false;
    }

    if (packet->node_id !=
        NODE_CENTRAL) {

        ESP_LOGW(
            TAG,
            "Sai time node_id: %u",
            packet->node_id);

        return false;
    }

    if (!valid_datetime(
            &packet->datetime)) {

        ESP_LOGW(
            TAG,
            "Datetime khong hop le");

        return false;
    }

    time_sync_packet_t
        temp = *packet;

    uint16_t received_crc =
        temp.crc;

    temp.crc = 0;

    uint16_t calculated_crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&temp,
            sizeof(temp) -
                sizeof(temp.crc));

    if (received_crc !=
        calculated_crc) {

        ESP_LOGW(
            TAG,
            "Time CRC FAIL: RX=0x%04X CALC=0x%04X",
            received_crc,
            calculated_crc);

        return false;
    }

    return true;
}

// =========================================================
// SEND STATUS
// =========================================================

static void send_actuator_status(void)
{
    node3_status_t snapshot;

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(100))
        != pdTRUE)
        return;

    snapshot = s_status;

    xSemaphoreGive(
        s_status_mutex);

    actuator_status_packet_t
        packet = {};

    packet.magic =
        ESPNOW_PACKET_MAGIC_STATUS;

    packet.room_temp =
        snapshot.room_temp;

    packet.datetime =
        snapshot.datetime;

    packet.node_id =
        NODE_ACTUATOR;

    packet.seq =
        snapshot.last_seq;

    packet.relay_mask =
        snapshot.relay_mask;

    packet.load_mask = 0;

    if (s_ct1_load_detected)
        packet.load_mask |=
            LOAD_1_BIT;

    if (s_ct2_load_detected)
        packet.load_mask |=
            LOAD_2_BIT;

    packet.ac_power =
        snapshot.is_power_on
            ? 1
            : 0;

    packet.target_temp =
        snapshot.target_temp;

    packet.control_mode =
        snapshot.control_mode;

    packet.crc = 0;

    packet.crc =
        esp_crc16_le(
            UINT16_MAX,
            (uint8_t const *)&packet,
            sizeof(packet) -
                sizeof(packet.crc));

    esp_err_t err =
        esp_now_send(
            s_central_mac,
            (uint8_t *)&packet,
            sizeof(packet));

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "esp_now_send status fail: %s",
            esp_err_to_name(err));
    }
}

// =========================================================
// ESP-NOW CALLBACK
// =========================================================

static void espnow_send_cb(
    const esp_now_send_info_t *tx_info,
    esp_now_send_status_t status)
{
    if (tx_info == NULL)
        return;

    ESP_LOGI(
        TAG,
        "Packet gui toi " MACSTR ", status: %s",
        MAC2STR(tx_info->des_addr),
        status == ESP_NOW_SEND_SUCCESS
            ? "SUCCESS"
            : "FAIL");
}

static void espnow_recv_cb(
    const esp_now_recv_info_t *recv_info,
    const uint8_t *data,
    int data_len)
{
    if (recv_info == NULL ||
        data == NULL)
        return;

    if (data_len <= 0 ||
        data_len >
            RX_DATA_MAX_SIZE) {

        ESP_LOGW(
            TAG,
            "RX packet size khong hop le: %d",
            data_len);

        return;
    }

    actuator_rx_event_t evt = {};

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
// PROCESS COMMAND
// =========================================================

static void process_command_packet(
    const actuator_command_packet_t *packet)
{
    if (!verify_command_packet(
            packet)) {

        ESP_LOGW(
            TAG,
            "Command packet bi loai");

        return;
    }

    bool send_ir = false;
    bool duplicate = false;

    int target_temp;
    bool power_on;

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(100))
        != pdTRUE)
        return;

    duplicate =
        packet->seq ==
        s_status.last_seq;

    if (!duplicate) {

        bool new_power =
            packet->ac_power != 0;

        // Chi gui IR khi:
        // - trang thai AC thay doi
        // - hoac AC dang ON va target thay doi
        if (s_status.is_power_on !=
                new_power ||
            (new_power &&
             s_status.target_temp !=
                 packet->target_temp)) {

            send_ir = true;
        }

        s_status.target_temp =
            packet->target_temp;

        s_status.is_power_on =
            new_power;

        s_status.relay_mask =
            packet->relay_mask;

        s_status.control_mode =
            packet->control_mode;

        s_status.last_seq =
            packet->seq;
    }

    target_temp =
        s_status.target_temp;

    power_on =
        s_status.is_power_on;

    xSemaphoreGive(
        s_status_mutex);

    if (duplicate) {
        ESP_LOGI(
            TAG,
            "Command seq=%u da xu ly, gui lai status",
            packet->seq);
    }
    else {
        ESP_LOGI(
            TAG,
            "Command OK | seq=%u | Relay=0x%02X | AC=%s | Temp=%u | Mode=%s",
            packet->seq,
            packet->relay_mask,
            packet->ac_power
                ? "ON"
                : "OFF",
            packet->target_temp,
            packet->control_mode ==
                    CONTROL_MODE_AUTO
                ? "AUTO"
                : "MANUAL");
    }

    if (send_ir) {
        send_ir_command(
            target_temp,
            power_on);
    }

    // Relay la command idempotent,
    // co the apply lai ngay ca duplicate.
    set_relays(
        packet->relay_mask);

    send_actuator_status();
}

// =========================================================
// PROCESS TIME SYNC
// =========================================================

static void process_time_sync_packet(
    const time_sync_packet_t *packet)
{
    if (!verify_time_sync_packet(
            packet)) {

        ESP_LOGW(
            TAG,
            "Time sync packet bi loai");

        return;
    }

    esp_err_t err =
        ds3231_set_datetime(
            &packet->datetime);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Khong the sync DS3231: %s",
            esp_err_to_name(err));

        return;
    }

    if (xSemaphoreTake(
            s_status_mutex,
            pdMS_TO_TICKS(100))
        == pdTRUE) {

        s_status.datetime =
            packet->datetime;

        xSemaphoreGive(
            s_status_mutex);
    }

    ESP_LOGI(
        TAG,
        "RTC synced: %04u-%02u-%02u %02u:%02u:%02u",
        packet->datetime.year,
        packet->datetime.month,
        packet->datetime.day,
        packet->datetime.hour,
        packet->datetime.minute,
        packet->datetime.second);

    send_actuator_status();
}

// =========================================================
// ESP-NOW RX TASK
// =========================================================

static void espnow_rx_task(
    void *pvParameter)
{
    actuator_rx_event_t evt;

    while (xQueueReceive(
               s_rx_queue,
               &evt,
               portMAX_DELAY)
           == pdTRUE) {

        if (memcmp(
                evt.src_mac,
                s_central_mac,
                ESP_NOW_ETH_ALEN)
            != 0) {

            ESP_LOGW(
                TAG,
                "Nhan packet tu node khong xac dinh: "
                MACSTR,
                MAC2STR(evt.src_mac));

            continue;
        }

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

        if (magic ==
                ESPNOW_PACKET_MAGIC_CMD &&
            evt.data_len ==
                sizeof(
                    actuator_command_packet_t)) {

            actuator_command_packet_t
                packet;

            memcpy(
                &packet,
                evt.data,
                sizeof(packet));

            ESP_LOGI(
                TAG,
                "RX COMMAND tu "
                MACSTR,
                MAC2STR(evt.src_mac));

            process_command_packet(
                &packet);
        }
        else if (
            magic ==
                ESPNOW_PACKET_MAGIC_TIME &&
            evt.data_len ==
                sizeof(
                    time_sync_packet_t)) {

            time_sync_packet_t
                packet;

            memcpy(
                &packet,
                evt.data,
                sizeof(packet));

            ESP_LOGI(
                TAG,
                "RX TIME SYNC tu "
                MACSTR,
                MAC2STR(evt.src_mac));

            process_time_sync_packet(
                &packet);
        }
        else {
            ESP_LOGW(
                TAG,
                "Packet khong xac dinh | Magic=0x%08lx | Len=%d",
                (unsigned long)magic,
                evt.data_len);
        }
    }
}

// =========================================================
// WIFI + ESP-NOW
// =========================================================

static esp_err_t init_wifi_espnow(void)
{
    s_rx_queue =
        xQueueCreate(
            ESPNOW_QUEUE_SIZE,
            sizeof(
                actuator_rx_event_t));

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

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(
        esp_wifi_set_storage(
            WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_start());

    ESP_ERROR_CHECK(
        esp_wifi_set_channel(
            ESPNOW_WIFI_CHANNEL,
            WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(
        esp_wifi_set_ps(
            WIFI_PS_NONE));

    uint8_t mac[
        ESP_NOW_ETH_ALEN];

    uint8_t channel;

    wifi_second_chan_t
        second_channel;

    ESP_ERROR_CHECK(
        esp_wifi_get_mac(
            WIFI_IF_STA,
            mac));

    ESP_ERROR_CHECK(
        esp_wifi_get_channel(
            &channel,
            &second_channel));

    ESP_LOGI(
        TAG,
        "Actuator STA MAC: "
        MACSTR,
        MAC2STR(mac));

    ESP_LOGI(
        TAG,
        "ESP-NOW channel: %u",
        channel);

    ESP_ERROR_CHECK(
        esp_now_init());

    ESP_ERROR_CHECK(
        esp_now_register_send_cb(
            espnow_send_cb));

    ESP_ERROR_CHECK(
        esp_now_register_recv_cb(
            espnow_recv_cb));

    esp_now_peer_info_t
        peer = {};

    memcpy(
        peer.peer_addr,
        s_central_mac,
        ESP_NOW_ETH_ALEN);

    peer.channel =
        ESPNOW_WIFI_CHANNEL;

    peer.ifidx =
        WIFI_IF_STA;

    peer.encrypt =
        false;

    if (!esp_now_is_peer_exist(
            s_central_mac)) {

        ESP_ERROR_CHECK(
            esp_now_add_peer(
                &peer));
    }

    return ESP_OK;
}

// =========================================================
// RELAY
// =========================================================

static void init_relays(void)
{
    gpio_config_t config = {};

    config.pin_bit_mask =
        (1ULL << RELAY_1_PIN) |
        (1ULL << RELAY_2_PIN);

    config.mode =
        GPIO_MODE_OUTPUT;

    config.pull_up_en =
        GPIO_PULLUP_DISABLE;

    config.pull_down_en =
        GPIO_PULLDOWN_DISABLE;

    config.intr_type =
        GPIO_INTR_DISABLE;

    ESP_ERROR_CHECK(
        gpio_config(&config));

    int off_level =
        !RELAY_ACTIVE_LEVEL;

    gpio_set_level(
        RELAY_1_PIN,
        off_level);

    gpio_set_level(
        RELAY_2_PIN,
        off_level);

    ESP_LOGI(
        TAG,
        "Relay initialized | IN1=GPIO%d | IN2=GPIO%d",
        RELAY_1_PIN,
        RELAY_2_PIN);
}

static void set_relays(
    uint8_t relay_mask)
{
    bool relay1_on =
        (relay_mask &
         RELAY_1_BIT) != 0;

    bool relay2_on =
        (relay_mask &
         RELAY_2_BIT) != 0;

    gpio_set_level(
        RELAY_1_PIN,
        relay1_on
            ? RELAY_ACTIVE_LEVEL
            : !RELAY_ACTIVE_LEVEL);

    gpio_set_level(
        RELAY_2_PIN,
        relay2_on
            ? RELAY_ACTIVE_LEVEL
            : !RELAY_ACTIVE_LEVEL);

    ESP_LOGI(
        TAG,
        "Relay output | R1=%s | R2=%s",
        relay1_on ? "ON" : "OFF",
        relay2_on ? "ON" : "OFF");
}

// =========================================================
// CURRENT SENSOR
// =========================================================

static esp_err_t init_current_sensors(void)
{
    adc_oneshot_unit_init_cfg_t
        init_config = {};

    init_config.unit_id =
        ADC_UNIT_1;

    init_config.ulp_mode =
        ADC_ULP_MODE_DISABLE;

    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &init_config,
            &s_adc_handle));

    adc_oneshot_chan_cfg_t
        channel_config = {};

    channel_config.bitwidth =
        ADC_BITWIDTH_DEFAULT;

    channel_config.atten =
        ADC_ATTEN_DB_12;

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            s_adc_handle,
            CT1_ADC_CHANNEL,
            &channel_config));

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            s_adc_handle,
            CT2_ADC_CHANNEL,
            &channel_config));

    ESP_LOGI(
        TAG,
        "Current sensors initialized | CT1=GPIO2/ADC1_CH2 | CT2=GPIO3/ADC1_CH3");

    return ESP_OK;
}

static void current_sensor_task(
    void *pvParameter)
{
    while (true) {

        int min1 = CT_ADC_MAX;
        int max1 = 0;

        int min2 = CT_ADC_MAX;
        int max2 = 0;

        int64_t start_us =
            esp_timer_get_time();

        int samples = 0;

        while (
            (esp_timer_get_time() -
             start_us) <
            (CT_SAMPLE_WINDOW_MS *
             1000LL)) {

            int raw1;
            int raw2;

            if (adc_oneshot_read(
                    s_adc_handle,
                    CT1_ADC_CHANNEL,
                    &raw1) ==
                ESP_OK) {

                if (raw1 < min1)
                    min1 = raw1;

                if (raw1 > max1)
                    max1 = raw1;
            }

            if (adc_oneshot_read(
                    s_adc_handle,
                    CT2_ADC_CHANNEL,
                    &raw2) ==
                ESP_OK) {

                if (raw2 < min2)
                    min2 = raw2;

                if (raw2 > max2)
                    max2 = raw2;
            }

            samples++;
        }

        s_ct1_p2p_raw =
            max1 - min1;

        s_ct2_p2p_raw =
            max2 - min2;

        const float alpha =
            0.25f;

        if (s_ct1_p2p_filtered ==
            0.0f) {

            s_ct1_p2p_filtered =
                s_ct1_p2p_raw;
        }
        else {
            s_ct1_p2p_filtered =
                alpha *
                    s_ct1_p2p_raw +
                (1.0f - alpha) *
                    s_ct1_p2p_filtered;
        }

        if (s_ct2_p2p_filtered ==
            0.0f) {

            s_ct2_p2p_filtered =
                s_ct2_p2p_raw;
        }
        else {
            s_ct2_p2p_filtered =
                alpha *
                    s_ct2_p2p_raw +
                (1.0f - alpha) *
                    s_ct2_p2p_filtered;
        }

        s_ct1_load_detected =
            s_ct1_p2p_filtered >=
            CT1_LOAD_THRESHOLD;

        s_ct2_load_detected =
            s_ct2_p2p_filtered >=
            CT2_LOAD_THRESHOLD;

        ESP_LOGI(
            TAG,
            "CT | CH1 raw=%d filtered=%.1f load=%s | CH2 raw=%d filtered=%.1f load=%s",
            s_ct1_p2p_raw,
            s_ct1_p2p_filtered,
            s_ct1_load_detected
                ? "ON"
                : "OFF",
            s_ct2_p2p_raw,
            s_ct2_p2p_filtered,
            s_ct2_load_detected
                ? "ON"
                : "OFF");

        if (min1 < 20 ||
            max1 > 4075 ||
            min2 < 20 ||
            max2 > 4075) {

            ESP_LOGW(
                TAG,
                "CT ADC gan clipping - kiem tra bien do tin hieu");
        }

        vTaskDelay(
            pdMS_TO_TICKS(880));
    }
}

// =========================================================
// BUTTONS
// =========================================================

static void init_buttons(void)
{
    gpio_config_t config = {};

    config.mode =
        GPIO_MODE_INPUT;

    config.pull_up_en =
        GPIO_PULLUP_ENABLE;

    config.pull_down_en =
        GPIO_PULLDOWN_DISABLE;

    config.intr_type =
        GPIO_INTR_DISABLE;

    config.pin_bit_mask =
        (1ULL <<
            BTN_TEMP_UP_PIN) |
        (1ULL <<
            BTN_TEMP_DOWN_PIN) |
        (1ULL <<
            BTN_AC_TOGGLE_PIN);

    ESP_ERROR_CHECK(
        gpio_config(&config));
}

static void button_task(
    void *pvParameter)
{
    int last_up = 1;
    int last_down = 1;
    int last_power = 1;

    while (true) {

        int up =
            gpio_get_level(
                BTN_TEMP_UP_PIN);

        int down =
            gpio_get_level(
                BTN_TEMP_DOWN_PIN);

        int power =
            gpio_get_level(
                BTN_AC_TOGGLE_PIN);

        bool changed = false;

        int target_temp = 25;
        bool power_on = false;

        if (up == 0 &&
            last_up == 1) {

            vTaskDelay(
                pdMS_TO_TICKS(50));

            if (gpio_get_level(
                    BTN_TEMP_UP_PIN)
                == 0) {

                xSemaphoreTake(
                    s_status_mutex,
                    portMAX_DELAY);

                if (s_status.target_temp <
                    30) {

                    s_status.target_temp++;
                }

                s_status.control_mode =
                    CONTROL_MODE_MANUAL;

                target_temp =
                    s_status.target_temp;

                power_on =
                    s_status.is_power_on;

                changed = true;

                xSemaphoreGive(
                    s_status_mutex);
            }
        }

        if (down == 0 &&
            last_down == 1) {

            vTaskDelay(
                pdMS_TO_TICKS(50));

            if (gpio_get_level(
                    BTN_TEMP_DOWN_PIN)
                == 0) {

                xSemaphoreTake(
                    s_status_mutex,
                    portMAX_DELAY);

                if (s_status.target_temp >
                    16) {

                    s_status.target_temp--;
                }

                s_status.control_mode =
                    CONTROL_MODE_MANUAL;

                target_temp =
                    s_status.target_temp;

                power_on =
                    s_status.is_power_on;

                changed = true;

                xSemaphoreGive(
                    s_status_mutex);
            }
        }

        if (power == 0 &&
            last_power == 1) {

            vTaskDelay(
                pdMS_TO_TICKS(50));

            if (gpio_get_level(
                    BTN_AC_TOGGLE_PIN)
                == 0) {

                xSemaphoreTake(
                    s_status_mutex,
                    portMAX_DELAY);

                s_status.is_power_on =
                    !s_status.is_power_on;

                s_status.control_mode =
                    CONTROL_MODE_MANUAL;

                target_temp =
                    s_status.target_temp;

                power_on =
                    s_status.is_power_on;

                changed = true;

                xSemaphoreGive(
                    s_status_mutex);
            }
        }

        if (changed) {

            ESP_LOGI(
                TAG,
                "Local control | AC=%s | Temp=%d | Mode=MANUAL",
                power_on
                    ? "ON"
                    : "OFF",
                target_temp);

            send_ir_command(
                target_temp,
                power_on);

            send_actuator_status();
        }

        last_up = up;
        last_down = down;
        last_power = power;

        vTaskDelay(
            pdMS_TO_TICKS(30));
    }
}

// =========================================================
// DISPLAY + RTC
// =========================================================

static void display_and_sensor_task(
    void *pvParameter)
{
    char line[32];

    while (true) {

        rtc_datetime_t
            datetime = {};

        float room_temp =
            0.0f;

        if (ds3231_read_datetime(
                &datetime,
                &room_temp) ==
            ESP_OK) {

            xSemaphoreTake(
                s_status_mutex,
                portMAX_DELAY);

            s_status.datetime =
                datetime;

            s_status.room_temp =
                room_temp;

            node3_status_t
                snapshot =
                    s_status;

            xSemaphoreGive(
                s_status_mutex);

            oled_clear();

            oled_draw_string(
                0,
                0,
                "ACTUATOR NODE");

            snprintf(
                line,
                sizeof(line),
                "%04u-%02u-%02u %02u:%02u:%02u",
                snapshot.datetime.year,
                snapshot.datetime.month,
                snapshot.datetime.day,
                snapshot.datetime.hour,
                snapshot.datetime.minute,
                snapshot.datetime.second);

            oled_draw_string(
                0,
                2,
                line);

            snprintf(
                line,
                sizeof(line),
                "TEMP %.1f C",
                snapshot.room_temp);

            oled_draw_string(
                0,
                4,
                line);

            if (snapshot.is_power_on) {

                snprintf(
                    line,
                    sizeof(line),
                    "AC ON SET %d C",
                    snapshot.target_temp);
            }
            else {
                snprintf(
                    line,
                    sizeof(line),
                    "AC OFF SET %d C",
                    snapshot.target_temp);
            }

            oled_draw_string(
                0,
                6,
                line);

            esp_err_t err =
                oled_render();

            if (err != ESP_OK) {

                ESP_LOGW(
                    TAG,
                    "OLED render fail: %s",
                    esp_err_to_name(err));
            }
        }
        else {
            ESP_LOGW(
                TAG,
                "Khong doc duoc DS3231");
        }

        vTaskDelay(
            pdMS_TO_TICKS(500));
    }
}

// =========================================================
// PERIODIC STATUS
// =========================================================

static void status_task(
    void *pvParameter)
{
    vTaskDelay(
        pdMS_TO_TICKS(1000));

    while (true) {

        send_actuator_status();

        vTaskDelay(
            pdMS_TO_TICKS(2000));
    }
}

// =========================================================
// APP MAIN
// =========================================================

extern "C" void app_main(void)
{
    esp_err_t ret =
        nvs_flash_init();

    if (ret ==
            ESP_ERR_NVS_NO_FREE_PAGES ||
        ret ==
            ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase());

        ret =
            nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Default state
    s_status.target_temp = 25;
    s_status.is_power_on = false;
    s_status.room_temp = 0.0f;
    s_status.relay_mask = 0;
    s_status.control_mode =
        CONTROL_MODE_AUTO;
    s_status.last_seq = 0;

    s_status_mutex =
        xSemaphoreCreateMutex();

    s_ir_mutex =
        xSemaphoreCreateMutex();

    if (!s_status_mutex ||
        !s_ir_mutex) {

        ESP_LOGE(
            TAG,
            "Khong tao duoc mutex");

        return;
    }

    ESP_ERROR_CHECK(
        init_i2c_bus());

    ESP_ERROR_CHECK(
        oled_init());

    ESP_ERROR_CHECK(
        ir_transmitter_init());

    init_buttons();
    init_relays();

    ESP_ERROR_CHECK(
        init_current_sensors());

    ESP_ERROR_CHECK(
        init_wifi_espnow());

    xTaskCreate(
        espnow_rx_task,
        "espnow_rx",
        4096,
        NULL,
        5,
        NULL);

    xTaskCreate(
        button_task,
        "button_task",
        3072,
        NULL,
        4,
        NULL);

    xTaskCreate(
        display_and_sensor_task,
        "display_task",
        4096,
        NULL,
        3,
        NULL);

    xTaskCreate(
        status_task,
        "status_task",
        3072,
        NULL,
        3,
        NULL);

    xTaskCreate(
        current_sensor_task,
        "current_sensor",
        3072,
        NULL,
        3,
        NULL);

    ESP_LOGI(
        TAG,
        "Actuator Node ready | Mode=AUTO");
}