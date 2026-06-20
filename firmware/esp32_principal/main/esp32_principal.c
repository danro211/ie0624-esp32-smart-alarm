#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi_secrets.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          10

static const char *TAG = "ESP32_NTP";
static EventGroupHandle_t wifi_event_group;
static int retry_count = 0;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Iniciando conexion WiFi...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            retry_count++;
            ESP_LOGW(TAG, "WiFi desconectado. Reintentando... intento %d/%d",
                     retry_count, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

        ESP_LOGI(TAG, "WiFi conectado");
        ESP_LOGI(TAG, "IP obtenida: " IPSTR, IP2STR(&event->ip_info.ip));

        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = { 0 };

    strncpy((char *) wifi_config.sta.ssid,
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid));

    strncpy((char *) wifi_config.sta.password,
            WIFI_PASS,
            sizeof(wifi_config.sta.password));

    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Configuracion WiFi terminada. Esperando conexion...");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado a la red: %s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "No se pudo conectar a la red: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Evento WiFi inesperado");
    }
}

static void sync_time_ntp(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");

    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    esp_netif_sntp_init(&sntp_config);

    ESP_LOGI(TAG, "Esperando sincronizacion de hora por NTP...");

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Hora sincronizada correctamente");
    } else {
        ESP_LOGW(TAG, "No se pudo sincronizar la hora dentro del tiempo esperado");
    }

    /*
     * Costa Rica usa UTC-6 y no aplica horario de verano.
     * En formato POSIX, "CST6" representa UTC-6.
     */
    setenv("TZ", "CST6", 1);
    tzset();
}

static void print_current_time(void)
{
    time_t now;
    struct tm timeinfo;
    char time_string[64];

    time(&now);
    localtime_r(&now, &timeinfo);

    strftime(time_string,
             sizeof(time_string),
             "%A, %d/%m/%Y %H:%M:%S",
             &timeinfo);

    ESP_LOGI(TAG, "Hora actual Costa Rica: %s", time_string);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Nodo principal iniciado");

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    wifi_init_sta();
    sync_time_ntp();

    while (1) {
        print_current_time();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
