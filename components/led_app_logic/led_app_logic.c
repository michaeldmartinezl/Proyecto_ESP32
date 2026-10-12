#include "led_app_logic.h"

const char *state_to_str(app_state_t s) {
    switch (s) {
        case STATE_PROVISIONING: return "provisioning";
        case STATE_CONNECTING:   return "connecting";
        case STATE_CONNECTED:    return "connected";
        case STATE_ERROR:        return "error";
        case STATE_CUSTOM:       return "custom";
        default:                 return "desconocido";
    }
}