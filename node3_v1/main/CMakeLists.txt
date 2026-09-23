idf_component_register(SRCS "main.cpp"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi ir_transmitter driver freertos esp_adc
                    INCLUDE_DIRS "." "include")