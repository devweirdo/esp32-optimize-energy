idf_component_register(SRCS "main.cpp"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_psram esp_timer
                    INCLUDE_DIRS "." "include")
