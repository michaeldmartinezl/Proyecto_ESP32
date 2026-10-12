#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include <esp_http_server.h>
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdint.h>
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "led_app_logic.h"  // app_state_t y state_to_str() (movidos a su propio componente para poder probarlos con Unity)

// --- VARIABLES GLOBALES (Compartidas entre red y hardware) ---
// Inician en verde fijo según la rúbrica
uint8_t current_r = 0;
uint8_t current_g = 255;
uint8_t current_b = 0;
bool is_blinking = false;
volatile uint16_t blink_period_ms = 500;   // periodo de parpadeo del color personalizado
static const char *last_button = "ninguno";

static const char *TAG = "PRACTICA_IOT";

// --- MÁQUINA DE ESTADOS ---
// (app_state_t ahora vive en components/led_app_logic/include/led_app_logic.h)
volatile app_state_t app_state = STATE_PROVISIONING;

// Pines
#define BOOT_BUTTON_GPIO 0
#define WS2812_GPIO      14    // pin de datos del WS2812B
#define WS2812_LED_COUNT 1

// Interfaz de red (para leer la IP desde la API)
static esp_netif_t *s_sta_netif = NULL;

// --- TAREAS DEL NÚCLEO 1 (HARDWARE) ---
// Driver WS2812B propio sobre el periférico RMT (sin depender del componente
// gestionado "led_strip", que aún no es compatible con este ESP-IDF).

#define WS2812_RMT_RESOLUTION_HZ 10000000  // 10 MHz -> 1 tick = 0.1 us

// Encoder mínimo: manda los bytes RGB con la temporización de WS2812
// y luego el pulso de reset (>= 50us en bajo).
typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                             const void *data, size_t data_size, rmt_encode_state_t *ret_state) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes_enc = enc->bytes_encoder;
    rmt_encoder_handle_t copy_enc = enc->copy_encoder;
    rmt_encode_state_t session_state = 0;
    rmt_encode_state_t state = 0;
    size_t encoded = 0;

    if (enc->state == 0) {
        encoded += bytes_enc->encode(bytes_enc, channel, data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session_state & RMT_ENCODING_MEM_FULL) { state |= RMT_ENCODING_MEM_FULL; goto out; }
    }
    if (enc->state == 1) {
        encoded += copy_enc->encode(copy_enc, channel, &enc->reset_code, sizeof(enc->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) { enc->state = 0; state |= RMT_ENCODING_COMPLETE; }
        if (session_state & RMT_ENCODING_MEM_FULL) { state |= RMT_ENCODING_MEM_FULL; goto out; }
    }
out:
    *ret_state = state;
    return encoded;
}

static esp_err_t ws2812_encoder_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_encoder_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_led_encoder = NULL;

static void ws2812_init(void) {
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &s_led_chan));

    ws2812_encoder_t *enc = calloc(1, sizeof(ws2812_encoder_t));
    enc->base.encode = ws2812_encode;
    enc->base.del = ws2812_encoder_del;
    enc->base.reset = ws2812_encoder_reset;

    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },   // 0.3us / 0.9us
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },   // 0.9us / 0.3us
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_encoder_config, &enc->bytes_encoder));

    rmt_copy_encoder_config_t copy_encoder_config;
    memset(&copy_encoder_config, 0, sizeof(copy_encoder_config));
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_encoder_config, &enc->copy_encoder));

    enc->reset_code = (rmt_symbol_word_t){ .level0 = 0, .duration0 = 250, .level1 = 0, .duration1 = 250 }; // 50us

    s_led_encoder = &enc->base;
    ESP_ERROR_CHECK(rmt_enable(s_led_chan));
}

static void ws2812_show(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t grb[3] = { g, r, b };  // el WS2812B recibe los bytes en orden G,R,B
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_encoder, grb, sizeof(grb), &tx_config);
    rmt_tx_wait_all_done(s_led_chan, portMAX_DELAY);
}

static void ws2812_off(void) {
    ws2812_show(0, 0, 0);
}

static void ws2812_pulse(uint8_t r, uint8_t g, uint8_t b, int on_ms, int off_ms) {
    ws2812_show(r, g, b);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    ws2812_off();
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

void led_task(void *pvParameters) {
    ws2812_init();

    while (1) {
        switch (app_state) {
            case STATE_PROVISIONING:            // azul lento
                ws2812_pulse(0, 0, 255, 1000, 1000);
                break;
            case STATE_CONNECTING:              // ámbar rápido
                ws2812_pulse(255, 170, 0, 150, 150);
                break;
            case STATE_CONNECTED:               // verde fijo
                ws2812_show(0, 255, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case STATE_ERROR:                   // rojo, doble pulso
                ws2812_pulse(255, 0, 0, 100, 100);
                ws2812_pulse(255, 0, 0, 100, 700);
                break;
            case STATE_CUSTOM:                  // lo que elija la web
                if (is_blinking) {
                    ws2812_pulse(current_r, current_g, current_b, blink_period_ms, blink_period_ms);
                } else {
                    ws2812_show(current_r, current_g, current_b);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                break;
        }
    }
}

// Vuelve al verde fijo (valor inicial de la rúbrica)
static void led_revert(void) {
    current_r = 0; current_g = 255; current_b = 0;
    is_blinking = false;
    blink_period_ms = 500;
    if (app_state == STATE_CUSTOM) app_state = STATE_CONNECTED;
    ESP_LOGI(TAG, "LED revertido a verde fijo");
}

#define LONG_PRESS_MS 3000   // pulsación larga: borrar credenciales
#define DEBOUNCE_MS   50     // menos que esto se ignora (rebote)
#define POLL_MS       20

void button_task(void *pvParameters) {
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    while (1) {
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {      // botón presionado
            int held_ms = 0;
            bool long_done = false;

            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {  // esperar a que lo suelte
                vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                held_ms += POLL_MS;
                if (!long_done && held_ms >= LONG_PRESS_MS) {
                    long_done = true;
                    last_button = "largo (borrando WiFi)";
                    ESP_LOGW(TAG, "BOOT largo: borrando credenciales WiFi (suelta el botón)");
                    esp_wifi_restore();
                }
            }

            if (long_done) {
                ESP_LOGW(TAG, "Reiniciando para volver a aprovisionar...");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (held_ms >= DEBOUNCE_MS) {
                last_button = "corto (revertido)";
                led_revert();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// --- NTP ---
static bool s_ntp_started = false;

static void ntp_start(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    s_ntp_started = true;
    ESP_LOGI(TAG, "Cliente NTP iniciado (pool.ntp.org)");
}

// --- SERVIDOR HTTP Y API REST ---
// (state_to_str() ahora vive en components/led_app_logic — ver test_apps/ ahí para su prueba unitaria)

// 1. GET /api/status
esp_err_t api_status_get_handler(httpd_req_t *req) {
    time_t now;
    time(&now);
    char time_str[32];
    if (now > 1700000000) {   // umbral: solo es una fecha real si ya sincronizó NTP
        struct tm tm_utc;
        gmtime_r(&now, &tm_utc);
        strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    } else {
        snprintf(time_str, sizeof(time_str), "sin sincronizar");
    }

    char ip_str[16] = "0.0.0.0";
    if (s_sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", state_to_str(app_state));
    cJSON_AddStringToObject(root, "ip", ip_str);
    cJSON_AddStringToObject(root, "time", time_str);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddStringToObject(root, "last_button", last_button);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// 2. GET /api/led
esp_err_t api_led_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "r", current_r);
    cJSON_AddNumberToObject(root, "g", current_g);
    cJSON_AddNumberToObject(root, "b", current_b);
    cJSON_AddBoolToObject(root, "blinking", is_blinking);
    cJSON_AddNumberToObject(root, "rate_ms", blink_period_ms);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

// Solo se acepta controlar el LED si ya hay WiFi (Conectado o Personalizado)
static bool device_is_online(void) {
    return app_state == STATE_CONNECTED || app_state == STATE_CUSTOM;
}

// 3. POST /api/led
esp_err_t api_led_post_handler(httpd_req_t *req) {
    if (!device_is_online()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"dispositivo no conectado\"}");
        return ESP_OK;
    }

    char buf[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cuerpo invalido");
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return ESP_FAIL;
    }

    cJSON *jr = cJSON_GetObjectItem(root, "r");
    cJSON *jg = cJSON_GetObjectItem(root, "g");
    cJSON *jb = cJSON_GetObjectItem(root, "b");
    cJSON *jblink = cJSON_GetObjectItem(root, "blinking");
    cJSON *jrate = cJSON_GetObjectItem(root, "rate_ms");

    if (!cJSON_IsNumber(jr) || !cJSON_IsNumber(jg) || !cJSON_IsNumber(jb)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Faltan r,g,b");
        return ESP_FAIL;
    }

    current_r = (uint8_t)jr->valueint;
    current_g = (uint8_t)jg->valueint;
    current_b = (uint8_t)jb->valueint;
    is_blinking = cJSON_IsTrue(jblink) || (cJSON_IsNumber(jblink) && jblink->valueint > 0);
    if (cJSON_IsNumber(jrate) && jrate->valueint >= 50) {
        blink_period_ms = (uint16_t)jrate->valueint;
    }
    app_state = STATE_CUSTOM;

    ESP_LOGI(TAG, "API REST: color R:%d G:%d B:%d blink:%d rate:%dms",
             current_r, current_g, current_b, is_blinking, blink_period_ms);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// 4. POST /api/led/revert
esp_err_t api_led_revert_post_handler(httpd_req_t *req) {
    if (!device_is_online()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"dispositivo no conectado\"}");
        return ESP_OK;
    }
    led_revert();
    last_button = "ninguno (revertido por API)";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// 5. GET / (página web embebida, ver main/page.html)
extern const uint8_t page_html_start[] asm("_binary_page_html_start");
extern const uint8_t page_html_end[]   asm("_binary_page_html_end");

esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)page_html_start, page_html_end - page_html_start);
    return ESP_OK;
}

// Función para arrancar el servidor web (solo una vez)
static httpd_handle_t s_server = NULL;

httpd_handle_t start_webserver(void) {
    if (s_server != NULL) return s_server;   // ya está corriendo

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler, .user_ctx = NULL };
        httpd_uri_t uri_get_led = { .uri = "/api/led", .method = HTTP_GET, .handler = api_led_get_handler, .user_ctx = NULL };
        httpd_uri_t uri_post_led = { .uri = "/api/led", .method = HTTP_POST, .handler = api_led_post_handler, .user_ctx = NULL };
        httpd_uri_t uri_revert_led = { .uri = "/api/led/revert", .method = HTTP_POST, .handler = api_led_revert_post_handler, .user_ctx = NULL };
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL };

        httpd_register_uri_handler(server, &uri_status);
        httpd_register_uri_handler(server, &uri_get_led);
        httpd_register_uri_handler(server, &uri_post_led);
        httpd_register_uri_handler(server, &uri_revert_led);
        httpd_register_uri_handler(server, &uri_root);

        s_server = server;
        ESP_LOGI(TAG, "Servidor HTTP iniciado en el puerto 80");
        return server;
    }
    return NULL;
}

// --- EVENTOS DE RED: cambian el estado según lo que pasa ---
static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == NETWORK_PROV_EVENT) {
        switch (id) {
            case NETWORK_PROV_WIFI_CRED_RECV:
                ESP_LOGI(TAG, "Credenciales recibidas");
                app_state = STATE_CONNECTING;
                break;
            case NETWORK_PROV_WIFI_CRED_FAIL:
                ESP_LOGE(TAG, "Credenciales WiFi incorrectas o red inalcanzable");
                app_state = STATE_ERROR;
                break;
            case NETWORK_PROV_END:
                network_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (app_state == STATE_CONNECTED) app_state = STATE_CONNECTING; // se perdió la red
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&ev->ip_info.ip));
        app_state = STATE_CONNECTED;
        start_webserver();
        if (!s_ntp_started) {
            ntp_start();
        }
    }
}

// --- TAREAS DEL NÚCLEO 0 (RED Y SERVICIOS) ---
void prov_task(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando aprovisionamiento BLE...");

    // 1. Inicializar la tarjeta de red WiFi
    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 2. Configurar el Gestor de Aprovisionamiento por Bluetooth
    network_prov_mgr_config_t manager_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(manager_config));

    // 3. Comprobar si ya hay credenciales guardadas en la memoria NVS
    bool provisioned = false;
    ESP_ERROR_CHECK(network_prov_mgr_is_wifi_provisioned(&provisioned));

    if (!provisioned) {
        ESP_LOGI(TAG, "Encendiendo Bluetooth (BLE) para recibir WiFi...");
        app_state = STATE_PROVISIONING;

        // Seguridad simple con PIN (POP)
        network_prov_security_t security = NETWORK_PROV_SECURITY_1;
        const char *pop = "123456789"; // Proof of Possession (POP)
        const char *service_name = "ESP32_MAICOL";

        ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(security, (const void *)pop, service_name, NULL));

        ESP_LOGW(TAG, "-> Abre la app 'ESP BLE Provisioning' en tu celular.");
        ESP_LOGW(TAG, "-> Conéctate al Bluetooth '%s' usando el PIN: %s", service_name, pop);
    } else {
        ESP_LOGI(TAG, "Credenciales WiFi encontradas en memoria. Conectando...");
        app_state = STATE_CONNECTING;
        network_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    vTaskDelete(NULL); // Termina la tarea, la red continúa sola en segundo plano
}

void net_task(void *pvParameters) {
    ESP_LOGI(TAG, "Iniciando servicios de red (NTP/mDNS)...");

    // El servidor web y el cliente NTP arrancan solos cuando llega la IP (ver event_handler).
    // mDNS se añadirá en el siguiente paso.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "--- Sistema Iniciado ---");

    // Inicializar NVS (Obligatorio para guardar claves de WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Inicializar pila de red TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Registrar los eventos de red (antes de lanzar las tareas)
    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Lanzar tareas
    xTaskCreatePinnedToCore(led_task, "led_task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(button_task, "button_task", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(prov_task, "prov_task", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(net_task, "net_task", 8192, NULL, 5, NULL, 0);
}