idf_component_register(SRCS "test_main.c" "test_led_app_logic.c"
                    INCLUDE_DIRS "."
                    REQUIRES unity led_app_logic
                    WHOLE_ARCHIVE)