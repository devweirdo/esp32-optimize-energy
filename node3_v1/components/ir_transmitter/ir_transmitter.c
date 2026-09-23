#include "ir_transmitter.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include <string.h>

static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

esp_err_t ir_transmitter_init(void)
{
    // 1. Create TX Channel
    rmt_tx_channel_config_t tx_channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = IR_TX_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = IR_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_config, &tx_channel));

    // 2. Configure 38kHz Carrier Wave
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = 0.33,
        .frequency_hz = 38000, 
        .flags = { .polarity_active_low = false }
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_cfg));

    // 3. Create raw copy encoder
    rmt_copy_encoder_config_t copy_encoder_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &copy_encoder));

    // 4. Enable Channel
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
    return ESP_OK;
}

// Generates the Mitsubishi payload dynamically based on our decoded formula
void set_ac_state(ir_ac_data_t *ac_data, int temp_c, bool power_on) 
{
    ac_data->length = AC_PAYLOAD_BYTES;
    
    // The static first 9 bytes
    uint8_t base_payload[9] = {0x52, 0xAE, 0xC3, 0x26, 0xD9, 0xFF, 0x00, 0x6F, 0x90};
    memcpy(ac_data->bytes, base_payload, 9);
    
    // Construct Byte 9: Temperature (upper nibble) + Power (lower nibble)
    uint8_t temp_nibble = 32 - temp_c;
    uint8_t power_nibble = power_on ? 0x06 : 0x0E;
    
    uint8_t byte9 = (temp_nibble << 4) | power_nibble;
    
    // Assign Byte 9 and its checksum (Byte 10)
    ac_data->bytes[9] = byte9;
    ac_data->bytes[10] = (uint8_t)(~byte9); // Bitwise NOT
}

static void set_symbol(rmt_symbol_word_t *sym, uint32_t d0, uint32_t l0, uint32_t d1, uint32_t l1) {
    sym->duration0 = d0; sym->level0 = l0;
    sym->duration1 = d1; sym->level1 = l1;
}

void ir_ac_transmit(const ir_ac_data_t *ac_data)
{
    if (!tx_channel || !ac_data || ac_data->length == 0) return;

    size_t max_symbols = 1 + (ac_data->length * 8) + 1;
    rmt_symbol_word_t *symbols = malloc(max_symbols * sizeof(rmt_symbol_word_t));
    
    int idx = 0;
    
    // Header: 3200us pulse, 1600us space
    set_symbol(&symbols[idx++], 3200, 1, 1600, 0);

    // Data Bits
    for (int i = 0; i < ac_data->length; i++) {
        for (int bit = 0; bit < 8; bit++) {
            bool is_one = (ac_data->bytes[i] & (1 << bit)) != 0;
            if (is_one) {
                set_symbol(&symbols[idx++], 400, 1, 1200, 0); // Bit 1
            } else {
                set_symbol(&symbols[idx++], 400, 1, 400, 0);  // Bit 0
            }
        }
    }

    // Stop Bit: 400us pulse, 10000us space
    set_symbol(&symbols[idx++], 400, 1, 10000, 0);

    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    ESP_ERROR_CHECK(rmt_transmit(tx_channel, copy_encoder, symbols, idx * sizeof(rmt_symbol_word_t), &tx_config));
    
    rmt_tx_wait_all_done(tx_channel, -1);
    free(symbols);
}