idf_component_register(SRCS "mqtt.c" "ethernet.c" "sc16is752.c" "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp32-component-ld2410c mqtt esp_eth esp_netif esp_event driver)
