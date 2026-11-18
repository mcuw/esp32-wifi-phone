idf_component_register(
    SRCS "main.c" "board.c" "storage.c" "net.c" "provision.c" "ota.c" "version.c"
         "peers.c" "call.c" "adpcm.c" "ui.c" "dns_server.c" "json_util.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer esp_http_server
                  esp_http_client esp_https_ota app_update esp-tls
                  esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_system
                  espcoredump esp_partition esp_app_format
)

# The setup page (web/dist) is compiled into the app, so that an OTA update
# brings the matching page along (issue #16). Build the web UI first:
#   (cd web && npm install && npm run build)
set(WEB_DIST ${CMAKE_CURRENT_SOURCE_DIR}/../web/dist)
set(WEB_PACK ${CMAKE_CURRENT_SOURCE_DIR}/../tools/pack_web.py)
set(WEB_C    ${CMAKE_CURRENT_BINARY_DIR}/web_files.c)
if(NOT EXISTS ${WEB_DIST}/index.html)
    message(WARNING "web/dist not found - run 'npm run build' in web/ to include the setup UI")
endif()
file(GLOB_RECURSE WEB_FILES CONFIGURE_DEPENDS ${WEB_DIST}/*)
idf_build_get_property(python PYTHON)
add_custom_command(
    OUTPUT ${WEB_C}
    COMMAND ${python} ${WEB_PACK} ${WEB_DIST} ${WEB_C}
    DEPENDS ${WEB_PACK} ${WEB_FILES}
    COMMENT "Embedding web/dist into the app"
    VERBATIM)
target_sources(${COMPONENT_LIB} PRIVATE ${WEB_C})
