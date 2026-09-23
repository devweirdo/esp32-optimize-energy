#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"

#include "espnow_protocol.h"
#include "model_data.h"

#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

static const char *TAG =
    "S3_FIRMWARE";

// =========================================================
// ESP-NOW
// =========================================================

static uint8_t
    s_central_mac[ESP_NOW_ETH_ALEN] = {
        0xff, 0xff, 0xff,
        0xff, 0xff, 0xff
    };

static QueueHandle_t
    s_espnow_queue = NULL;

// =========================================================
// CAMERA OV3660
// =========================================================

#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1

#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5

#define CAM_PIN_D7      16
#define CAM_PIN_D6      17
#define CAM_PIN_D5      18
#define CAM_PIN_D4      12
#define CAM_PIN_D3      10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0      11

#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK    13

// =========================================================
// MODEL
// =========================================================

#define MODEL_WIDTH     96
#define MODEL_HEIGHT    96
#define MODEL_PIXELS    (MODEL_WIDTH * MODEL_HEIGHT)

#define NUM_CLASSES     4

constexpr int
    kTensorArenaSize =
        120 * 1024;

static uint8_t
    *s_tensor_arena = NULL;

static const tflite::Model
    *s_model = nullptr;

static tflite::MicroInterpreter
    *s_interpreter = nullptr;

static TfLiteTensor
    *s_model_input = nullptr;

static TfLiteTensor
    *s_model_output = nullptr;

static const char
    *kLabels[NUM_CLASSES] = {
        "Empty",
        "Reading",
        "Sleep",
        "Active"
    };

// =========================================================
// TEMPORAL FILTER
// =========================================================

#define CONF_THRESHOLD       0.60f
#define REQUIRED_CONSECUTIVE 3

static int
    s_stable_class = -1;

static float
    s_stable_confidence = 0.0f;

static int
    s_candidate_class = -1;

static int
    s_candidate_count = 0;

// =========================================================
// ESP-NOW CALLBACK
// =========================================================

static void espnow_send_cb(
    const esp_now_send_info_t *tx_info,
    esp_now_send_status_t status)
{
    if (tx_info == NULL)
        return;

    espnow_event_t evt = {};

    evt.id =
        ESPNOW_SEND_CB;

    memcpy(
        evt.info.send_cb.mac_addr,
        tx_info->des_addr,
        ESP_NOW_ETH_ALEN);

    evt.info.send_cb.status =
        status;

    if (xQueueSend(
            s_espnow_queue,
            &evt,
            0) != pdTRUE) {

        ESP_LOGW(
            TAG,
            "Hang doi ESP-NOW gui bi day");
    }
}

// =========================================================
// ESP-NOW EVENT TASK
// =========================================================

static void espnow_event_task(
    void *pvParameter)
{
    espnow_event_t evt;

    while (xQueueReceive(
               s_espnow_queue,
               &evt,
               portMAX_DELAY)
           == pdTRUE) {

        if (evt.id !=
            ESPNOW_SEND_CB)
            continue;

        espnow_event_send_cb_t
            *send_cb =
                &evt.info.send_cb;

        ESP_LOGI(
            TAG,
            "Packet gui toi " MACSTR ", status: %s",
            MAC2STR(send_cb->mac_addr),
            send_cb->status ==
                    ESP_NOW_SEND_SUCCESS
                ? "SUCCESS"
                : "FAIL");
    }
}

// =========================================================
// ESP-NOW INIT
// =========================================================

static esp_err_t init_wifi_espnow(void)
{
    s_espnow_queue =
        xQueueCreate(
            ESPNOW_QUEUE_SIZE,
            sizeof(espnow_event_t));

    if (!s_espnow_queue) {

        ESP_LOGE(
            TAG,
            "Khong the tao ESP-NOW queue");

        return ESP_FAIL;
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

    ESP_ERROR_CHECK(
        esp_wifi_get_mac(
            WIFI_IF_STA,
            mac));

    ESP_LOGI(
        TAG,
        "Camera STA MAC: "
        MACSTR,
        MAC2STR(mac));

    ESP_ERROR_CHECK(
        esp_now_init());

    ESP_ERROR_CHECK(
        esp_now_register_send_cb(
            espnow_send_cb));

    esp_now_peer_info_t peer = {};

    peer.channel =
        ESPNOW_WIFI_CHANNEL;

    peer.ifidx =
        WIFI_IF_STA;

    peer.encrypt =
        false;

    memcpy(
        peer.peer_addr,
        s_central_mac,
        ESP_NOW_ETH_ALEN);

    if (!esp_now_is_peer_exist(
            s_central_mac)) {

        ESP_ERROR_CHECK(
            esp_now_add_peer(
                &peer));
    }

    xTaskCreate(
        espnow_event_task,
        "espnow_event_task",
        3072,
        NULL,
        4,
        NULL);

    return ESP_OK;
}

// =========================================================
// CAMERA INIT
// =========================================================

static esp_err_t init_camera(void)
{
    camera_config_t config = {};

    config.ledc_channel =
        LEDC_CHANNEL_0;

    config.ledc_timer =
        LEDC_TIMER_0;

    config.pin_d0 =
        CAM_PIN_D0;

    config.pin_d1 =
        CAM_PIN_D1;

    config.pin_d2 =
        CAM_PIN_D2;

    config.pin_d3 =
        CAM_PIN_D3;

    config.pin_d4 =
        CAM_PIN_D4;

    config.pin_d5 =
        CAM_PIN_D5;

    config.pin_d6 =
        CAM_PIN_D6;

    config.pin_d7 =
        CAM_PIN_D7;

    config.pin_xclk =
        CAM_PIN_XCLK;

    config.pin_pclk =
        CAM_PIN_PCLK;

    config.pin_vsync =
        CAM_PIN_VSYNC;

    config.pin_href =
        CAM_PIN_HREF;

    config.pin_sccb_sda =
        CAM_PIN_SIOD;

    config.pin_sccb_scl =
        CAM_PIN_SIOC;

    config.pin_pwdn =
        CAM_PIN_PWDN;

    config.pin_reset =
        CAM_PIN_RESET;

    config.xclk_freq_hz =
        20000000;

    // Camera output truc tiep
    // 96x96 Grayscale.
    config.frame_size =
        FRAMESIZE_96X96;

    config.pixel_format =
        PIXFORMAT_GRAYSCALE;

    config.grab_mode =
        CAMERA_GRAB_WHEN_EMPTY;

    config.fb_location =
        CAMERA_FB_IN_PSRAM;

    config.fb_count = 2;

    esp_err_t err =
        esp_camera_init(
            &config);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Khoi tao camera that bai: 0x%x",
            err);

        return err;
    }

    sensor_t *sensor =
        esp_camera_sensor_get();

    if (sensor != NULL) {

        // Horizontal mirror only.
        sensor->set_hmirror(
            sensor,
            1);
    }

    ESP_LOGI(
        TAG,
        "Camera warm-up: bo 10 frame dau");

    for (int i = 0;
         i < 10;
         i++) {

        camera_fb_t *fb =
            esp_camera_fb_get();

        if (fb != NULL) {
            esp_camera_fb_return(
                fb);
        }

        vTaskDelay(
            pdMS_TO_TICKS(100));
    }

    return ESP_OK;
}

// =========================================================
// TFLITE MICRO INIT
// =========================================================

static esp_err_t init_tflite_micro(void)
{
    tflite::InitializeTarget();

    s_model =
        tflite::GetModel(
            model_data);

    if (s_model->version() !=
        TFLITE_SCHEMA_VERSION) {

        ESP_LOGE(
            TAG,
            "Phien ban schema model khong khop!");

        return ESP_FAIL;
    }

    static
        tflite::MicroMutableOpResolver<9>
            resolver;

    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddMaxPool2D();
    resolver.AddSpaceToBatchNd();
    resolver.AddBatchToSpaceNd();
    resolver.AddAdd();
    resolver.AddMean();
    resolver.AddFullyConnected();
    resolver.AddSoftmax();

    s_tensor_arena =
        (uint8_t *)heap_caps_malloc(
            kTensorArenaSize,
            MALLOC_CAP_SPIRAM |
            MALLOC_CAP_8BIT);

    if (!s_tensor_arena) {

        ESP_LOGE(
            TAG,
            "Khong the cap phat %d bytes tren PSRAM!",
            kTensorArenaSize);

        return ESP_ERR_NO_MEM;
    }

    static
        tflite::MicroInterpreter
            static_interpreter(
                s_model,
                resolver,
                s_tensor_arena,
                kTensorArenaSize);

    s_interpreter =
        &static_interpreter;

    if (s_interpreter
            ->AllocateTensors()
        != kTfLiteOk) {

        ESP_LOGE(
            TAG,
            "AllocateTensors fail");

        return ESP_FAIL;
    }

    s_model_input =
        s_interpreter->input(0);

    s_model_output =
        s_interpreter->output(0);

    ESP_LOGI(
        TAG,
        "Input: type=%d scale=%f zero_point=%d",
        s_model_input->type,
        s_model_input->params.scale,
        s_model_input->params.zero_point);

    ESP_LOGI(
        TAG,
        "Output: type=%d scale=%f zero_point=%d",
        s_model_output->type,
        s_model_output->params.scale,
        s_model_output->params.zero_point);

    if (s_model_input->type !=
            kTfLiteInt8 ||
        s_model_output->type !=
            kTfLiteInt8) {

        ESP_LOGE(
            TAG,
            "Model khong phai INT8!");

        return ESP_FAIL;
    }

    if (
        s_model_input->dims == NULL ||
        s_model_input->dims->size != 4 ||
        s_model_input->dims->data[0] != 1 ||
        s_model_input->dims->data[1] !=
            MODEL_HEIGHT ||
        s_model_input->dims->data[2] !=
            MODEL_WIDTH ||
        s_model_input->dims->data[3] != 1) {

        ESP_LOGE(
            TAG,
            "Input tensor shape khong dung [1,%d,%d,1]",
            MODEL_HEIGHT,
            MODEL_WIDTH);

        return ESP_FAIL;
    }

    if (
        s_model_output->dims == NULL ||
        s_model_output->dims->size != 2 ||
        s_model_output->dims->data[0] != 1 ||
        s_model_output->dims->data[1] !=
            NUM_CLASSES) {

        ESP_LOGE(
            TAG,
            "Output tensor shape khong dung [1,%d]",
            NUM_CLASSES);

        return ESP_FAIL;
    }

    return ESP_OK;
}

// =========================================================
// TEMPORAL FILTER
// =========================================================

static bool update_stable_state(
    int raw_class,
    float raw_confidence)
{
    if (raw_confidence <
        CONF_THRESHOLD) {

        s_candidate_class = -1;
        s_candidate_count = 0;

        return false;
    }

    if (raw_class ==
        s_stable_class) {

        s_stable_confidence =
            raw_confidence;

        s_candidate_class = -1;
        s_candidate_count = 0;

        return false;
    }

    if (raw_class ==
        s_candidate_class) {

        s_candidate_count++;
    }
    else {
        s_candidate_class =
            raw_class;

        s_candidate_count = 1;
    }

    ESP_LOGI(
        TAG,
        "Candidate: %s (%d/%d)",
        kLabels[s_candidate_class],
        s_candidate_count,
        REQUIRED_CONSECUTIVE);

    if (s_candidate_count >=
        REQUIRED_CONSECUTIVE) {

        s_stable_class =
            s_candidate_class;

        s_stable_confidence =
            raw_confidence;

        s_candidate_class = -1;
        s_candidate_count = 0;

        ESP_LOGI(
            TAG,
            "Stable state changed -> %s",
            kLabels[s_stable_class]);

        return true;
    }

    return false;
}

// =========================================================
// AI INFERENCE TASK
// =========================================================

static void ai_inference_task(
    void *pvParameters)
{
    room_ai_packet_t packet = {};

    packet.magic =
        ESPNOW_PACKET_MAGIC_AI;

    packet.node_id =
        NODE_CAM_AI;

    while (true) {

        // =================================================
        // CAPTURE
        // =================================================

        camera_fb_t *fb =
            esp_camera_fb_get();

        if (!fb) {

            ESP_LOGE(
                TAG,
                "Loi chup anh (Buffer Busy)");

            vTaskDelay(
                pdMS_TO_TICKS(500));

            continue;
        }

        if (fb->len <
            MODEL_PIXELS) {

            ESP_LOGE(
                TAG,
                "Frame size invalid: %u bytes",
                (unsigned int)fb->len);

            esp_camera_fb_return(
                fb);

            vTaskDelay(
                pdMS_TO_TICKS(500));

            continue;
        }

        // =================================================
        // INPUT UINT8 -> INT8
        // =================================================

        for (int i = 0;
             i < MODEL_PIXELS;
             i++) {

            s_model_input
                ->data.int8[i] =
                (int8_t)(
                    (int16_t)
                        fb->buf[i] -
                    128);
        }

        esp_camera_fb_return(
            fb);

        // =================================================
        // INFERENCE
        // =================================================

        int64_t start_time =
            esp_timer_get_time();

        TfLiteStatus invoke_status =
            s_interpreter->Invoke();

        int64_t latency_us =
            esp_timer_get_time() -
            start_time;

        if (invoke_status !=
            kTfLiteOk) {

            ESP_LOGE(
                TAG,
                "Invoke fail");

            vTaskDelay(
                pdMS_TO_TICKS(1000));

            continue;
        }

        // =================================================
        // OUTPUT DEQUANTIZATION
        // =================================================

        const float out_scale =
            s_model_output
                ->params.scale;

        const int out_zero_point =
            s_model_output
                ->params.zero_point;

        float confidence[
            NUM_CLASSES];

        for (int i = 0;
             i < NUM_CLASSES;
             i++) {

            confidence[i] =
                (s_model_output
                     ->data.int8[i] -
                 out_zero_point) *
                out_scale;
        }

        int best_class = 0;

        float best_conf =
            confidence[0];

        for (int i = 1;
             i < NUM_CLASSES;
             i++) {

            if (confidence[i] >
                best_conf) {

                best_conf =
                    confidence[i];

                best_class = i;
            }
        }

        ESP_LOGI(
            TAG,
            "E:%5.1f%% R:%5.1f%% S:%5.1f%% A:%5.1f%% | Raw: %-7s %5.1f%% | Inference Latency: %lld ms",
            confidence[0] * 100.0f,
            confidence[1] * 100.0f,
            confidence[2] * 100.0f,
            confidence[3] * 100.0f,
            kLabels[best_class],
            best_conf * 100.0f,
            latency_us / 1000);

        // =================================================
        // TEMPORAL FILTER
        // =================================================

        update_stable_state(
            best_class,
            best_conf);

        if (s_stable_class < 0) {

            ESP_LOGI(
                TAG,
                "Stable state: chua xac dinh");

            vTaskDelay(
                pdMS_TO_TICKS(2000));

            continue;
        }

        ESP_LOGI(
            TAG,
            "Stable state: %-7s | Confidence: %5.1f%%",
            kLabels[s_stable_class],
            s_stable_confidence *
                100.0f);

        // =================================================
        // ESP-NOW PACKET
        // =================================================

        packet.room_state =
            (uint8_t)
                s_stable_class;

        packet.confidence =
            s_stable_confidence;

        packet.crc = 0;

        packet.crc =
            esp_crc16_le(
                UINT16_MAX,
                (uint8_t const *)&packet,
                sizeof(packet) -
                    sizeof(packet.crc));

        esp_err_t send_err =
            esp_now_send(
                s_central_mac,
                (uint8_t *)&packet,
                sizeof(packet));

        if (send_err !=
            ESP_OK) {

            ESP_LOGE(
                TAG,
                "Loi goi esp_now_send: %s",
                esp_err_to_name(
                    send_err));
        }

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

    ESP_ERROR_CHECK(
        init_wifi_espnow());

    ESP_ERROR_CHECK(
        init_camera());

    ESP_ERROR_CHECK(
        init_tflite_micro());

    xTaskCreatePinnedToCore(
        ai_inference_task,
        "ai_task",
        8192,
        NULL,
        5,
        NULL,
        1);

    ESP_LOGI(
        TAG,
        "AI Camera Node ready");
}