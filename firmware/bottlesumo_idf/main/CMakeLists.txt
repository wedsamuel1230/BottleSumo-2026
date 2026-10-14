idf_component_register(SRCS "robot.cpp" "wireless.cpp" "receiver.cpp"
    INCLUDE_DIRS "." "${PROJECT_DIR}/firmware/bottlesumo_competition"
    REQUIRES vl53l0x esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_adc
      esp_wifi esp_netif esp_http_server app_update nvs_flash esp_timer esp_system
      mbedtls esp_driver_usb_serial_jtag esp_psram)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
