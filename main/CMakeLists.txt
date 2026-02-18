idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver spi_flash json esp_http_client
                    PRIV_REQUIRES esp_app_format
)
