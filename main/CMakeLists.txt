idf_component_register(
    SRCS "main.c" "board.c" "storage.c" "net.c" "provision.c"
         "peers.c" "call.c" "adpcm.c" "ui.c" "dns_server.c" "json_util.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer esp_http_server
                  spiffs esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_system
                  espcoredump esp_partition esp_app_format
)

# Flash the built Astro/Qwik site into the "www" partition.
# Build the web UI first:  (cd web && npm install && npm run build)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../web/dist)
    spiffs_create_partition_image(www ${CMAKE_CURRENT_SOURCE_DIR}/../web/dist FLASH_IN_PROJECT)
else()
    message(WARNING "web/dist not found - run 'npm run build' in web/ to include the setup UI")
endif()
