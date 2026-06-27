#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_secrets.h"

/*
 * Pinout tipico AI Thinker ESP32-CAM.
 * En esta placa la camara fue detectada como OV3660 y funciona con este pinout.
 */
#define CAM_PIN_PWDN    32
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK     0
#define CAM_PIN_SIOD    26
#define CAM_PIN_SIOC    27

#define CAM_PIN_D7      35
#define CAM_PIN_D6      34
#define CAM_PIN_D5      39
#define CAM_PIN_D4      36
#define CAM_PIN_D3      21
#define CAM_PIN_D2      19
#define CAM_PIN_D1      18
#define CAM_PIN_D0       5
#define CAM_PIN_VSYNC   25
#define CAM_PIN_HREF    23
#define CAM_PIN_PCLK    22

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_WIFI_RETRY     10

static const char *TAG = "ESP32_CAM";

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

static bool camera_ready = false;
static uint16_t camera_pid = 0;
static size_t psram_size_bytes = 0;

static const char *INDEX_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32-CAM Smart Alarm</title>"
    "<style>"
    "body{font-family:Arial;margin:20px;background:#111;color:#eee;text-align:center;}"
    "img{max-width:95vw;border:3px solid #555;border-radius:8px;}"
    ".box{margin:12px auto;padding:12px;max-width:600px;background:#222;border-radius:8px;}"
    "a{color:#8fd3ff;}"
    "</style>"
    "</head>"
    "<body>"
    "<h1>ESP32-CAM Smart Alarm</h1>"
    "<div class=\"box\">"
    "<p>Vista de prueba de la camara. La imagen se actualiza cada segundo.</p>"
    "<img id=\"cam\" src=\"/capture\" alt=\"captura camara\">"
    "<p><a href=\"/capture\" target=\"_blank\">Abrir captura JPEG</a> | "
    "<a href=\"/status\" target=\"_blank\">Ver estado JSON</a></p>"
    "</div>"
    "<script>"
    "setInterval(function(){"
    "document.getElementById('cam').src='/capture?t=' + Date.now();"
    "},1000);"
    "</script>"
    "</body>"
    "</html>";

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Iniciando conexion WiFi...");
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < MAX_WIFI_RETRY) {
            wifi_retry_count++;
            ESP_LOGW(TAG,
                     "WiFi desconectado. Reintentando %d/%d...",
                     wifi_retry_count,
                     MAX_WIFI_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "WiFi conectado");
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Abra en el navegador: http://" IPSTR "/",
                 IP2STR(&event->ip_info.ip));

        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear grupo de eventos WiFi");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = { 0 };

    snprintf((char *)wifi_config.sta.ssid,
             sizeof(wifi_config.sta.ssid),
             "%s",
             WIFI_SSID);

    snprintf((char *)wifi_config.sta.password,
             sizeof(wifi_config.sta.password),
             "%s",
             WIFI_PASS);

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado a WiFi: %s", WIFI_SSID);
        return true;
    }

    ESP_LOGE(TAG, "No se pudo conectar a WiFi: %s", WIFI_SSID);
    return false;
}

static bool camera_init(void)
{
    psram_size_bytes = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    bool psram_available = psram_size_bytes > 0;

    ESP_LOGI(TAG, "Inicializando ESP32-CAM");
    ESP_LOGI(TAG, "PSRAM disponible: %s", psram_available ? "si" : "no");
    ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned int)psram_size_bytes);

    camera_config_t config = { 0 };

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;

    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;

    config.pin_xclk = CAM_PIN_XCLK;
    config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;

    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (psram_available) {
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size = FRAMESIZE_QQVGA;
        config.jpeg_quality = 15;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t ret = esp_camera_init(&config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error inicializando camara: 0x%x", ret);
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();

    if (sensor == NULL) {
        ESP_LOGE(TAG, "No se pudo obtener sensor");
        return false;
    }

    camera_pid = sensor->id.PID;

    ESP_LOGI(TAG, "Camara inicializada correctamente");
    ESP_LOGI(TAG, "PID del sensor: 0x%04x", camera_pid);

    camera_ready = true;
    return true;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!camera_ready) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Camara no disponible");
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "No se pudo capturar frame para HTTP");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "Error capturando frame");
    }

    ESP_LOGI(TAG,
             "HTTP capture | ancho=%d alto=%d bytes=%u",
             fb->width,
             fb->height,
             (unsigned int)fb->len);

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    esp_err_t ret = httpd_resp_send(req,
                                    (const char *)fb->buf,
                                    fb->len);

    esp_camera_fb_return(fb);

    return ret;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char response[256];

    snprintf(response,
             sizeof(response),
             "{"
             "\"camera_ready\":%s,"
             "\"sensor_pid\":\"0x%04x\","
             "\"psram_bytes\":%u"
             "}",
             camera_ready ? "true" : "false",
             camera_pid,
             (unsigned int)psram_size_bytes);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static bool start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;

    esp_err_t ret = httpd_start(&server, &config);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar servidor HTTP: 0x%x", ret);
        return false;
    }

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t capture_uri = {
        .uri = "/capture",
        .method = HTTP_GET,
        .handler = capture_handler,
        .user_ctx = NULL
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &capture_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));

    ESP_LOGI(TAG, "Servidor HTTP iniciado");
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Nodo ESP32-CAM iniciado");

    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    if (!wifi_connect()) {
        ESP_LOGE(TAG, "No se puede continuar sin WiFi");
        return;
    }

    if (!camera_init()) {
        ESP_LOGE(TAG, "No se puede continuar sin camara");
        return;
    }

    if (!start_web_server()) {
        ESP_LOGE(TAG, "No se puede continuar sin servidor HTTP");
        return;
    }

    ESP_LOGI(TAG, "ESP32-CAM lista");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
