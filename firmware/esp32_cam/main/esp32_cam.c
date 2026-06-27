#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * Pinout tipico para ESP32-CAM AI Thinker con camara OV2640.
 * Si la camara no inicializa, lo primero que se debe revisar es
 * si la placa usa este mismo pinout.
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

static const char *TAG = "ESP32_CAM";

static bool camera_init(void)
{
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    bool psram_available = psram_size > 0;

    ESP_LOGI(TAG, "Inicializando ESP32-CAM");
    ESP_LOGI(TAG, "PSRAM disponible: %s", psram_available ? "si" : "no");
    ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned int)psram_size);

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

    /*
     * Si hay PSRAM, usamos QVGA y 2 buffers.
     * Si no hay PSRAM, bajamos a QQVGA y 1 buffer en DRAM.
     */
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
        ESP_LOGE(TAG, "No se pudo obtener el sensor de la camara");
        return false;
    }

    ESP_LOGI(TAG, "Camara inicializada correctamente");
    ESP_LOGI(TAG, "PID del sensor: 0x%02x", sensor->id.PID);

    return true;
}

static void camera_capture_test(void)
{
    camera_fb_t *fb = esp_camera_fb_get();

    if (fb == NULL) {
        ESP_LOGE(TAG, "No se pudo capturar frame");
        return;
    }

    ESP_LOGI(TAG,
             "Frame OK | ancho=%d alto=%d bytes=%u formato=%d",
             fb->width,
             fb->height,
             (unsigned int)fb->len,
             fb->format);

    esp_camera_fb_return(fb);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Nodo ESP32-CAM iniciado");

    bool camera_ok = camera_init();

    if (!camera_ok) {
        ESP_LOGE(TAG, "La prueba de camara no puede continuar");

        while (true) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "Iniciando prueba de captura cada 3 segundos");

    while (true) {
        camera_capture_test();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
