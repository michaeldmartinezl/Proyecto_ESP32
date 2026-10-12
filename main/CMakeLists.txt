idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES "page.html"
                    REQUIRES nvs_flash esp_wifi esp_http_server esp_netif esp_timer driver esp_driver_rmt esp_driver_gpio led_app_logic network_provisioning cjson)