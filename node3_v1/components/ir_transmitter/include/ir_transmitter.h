#ifndef IR_TRANSMITTER_H
#define IR_TRANSMITTER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_TX_GPIO_NUM          GPIO_NUM_20
#define IR_RESOLUTION_HZ        1000000
#define AC_PAYLOAD_BYTES        11

#define BTN_TEMP_UP_PIN         GPIO_NUM_0
#define BTN_TEMP_DOWN_PIN       GPIO_NUM_1
#define BTN_AC_TOGGLE_PIN       GPIO_NUM_10

typedef struct {
    uint8_t bytes[AC_PAYLOAD_BYTES];
    uint8_t length;
} ir_ac_data_t;

esp_err_t ir_transmitter_init(void);
void ir_ac_transmit(const ir_ac_data_t *ac_data);
void set_ac_state(ir_ac_data_t *ac_data, int temp_c, bool power_on);

#ifdef __cplusplus
}
#endif

#endif // IR_TRANSMITTER_H