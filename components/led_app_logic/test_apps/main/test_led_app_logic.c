#include "unity.h"
#include "led_app_logic.h"

// Caso 1: cada estado conocido debe traducirse exactamente al texto
// que la API REST expone en /api/status (campo "status").
TEST_CASE("state_to_str devuelve el texto correcto para cada estado conocido", "[led_app_logic]")
{
    TEST_ASSERT_EQUAL_STRING("provisioning", state_to_str(STATE_PROVISIONING));
    TEST_ASSERT_EQUAL_STRING("connecting",   state_to_str(STATE_CONNECTING));
    TEST_ASSERT_EQUAL_STRING("connected",    state_to_str(STATE_CONNECTED));
    TEST_ASSERT_EQUAL_STRING("error",        state_to_str(STATE_ERROR));
    TEST_ASSERT_EQUAL_STRING("custom",       state_to_str(STATE_CUSTOM));
}

// Caso 2: un valor fuera del enum no debe devolver basura ni colgarse,
// debe caer en el "default" de la función.
TEST_CASE("state_to_str no revienta con un valor fuera de rango", "[led_app_logic]")
{
    TEST_ASSERT_EQUAL_STRING("desconocido", state_to_str((app_state_t)99));
}