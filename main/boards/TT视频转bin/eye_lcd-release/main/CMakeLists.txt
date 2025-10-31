idf_component_register(SRCS "eye_lcd.c" "esp_lcd_gc9d01n.c" "uart_echo_example_main.c" "button.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd esp_partition
                    )

