#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Estados posibles de la aplicación (mismo enum que antes vivía en main.c).
typedef enum {
    STATE_PROVISIONING,
    STATE_CONNECTING,
    STATE_CONNECTED,
    STATE_ERROR,
    STATE_CUSTOM
} app_state_t;

// Traduce un estado de la aplicación a su representación en texto,
// tal como se expone en el campo "status" de GET /api/status.
// Es una función pura (sin efectos secundarios), por eso es la más
// fácil de probar con Unity sin necesitar WiFi ni hardware real.
const char *state_to_str(app_state_t s);

#ifdef __cplusplus
}
#endif