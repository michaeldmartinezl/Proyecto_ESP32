#include "unity.h"

// Punto de entrada del proyecto de pruebas: Unity corre todos los
// TEST_CASE() que encuentre enlazados (ver test_led_app_logic.c)
// y muestra el resultado por el monitor serial.
void app_main(void) {
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
}