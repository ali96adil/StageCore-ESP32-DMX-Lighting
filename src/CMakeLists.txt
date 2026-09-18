idf_component_register(
    SRCS
        "main.cpp"
        "config_store.cpp"
        "device_identity.cpp"
        "provisioning.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_http_server
        esp_netif
        esp_wifi
        mbedtls
        nvs_flash
)
