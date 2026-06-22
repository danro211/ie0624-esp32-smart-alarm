#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include "alarmas_config.h"
#include "wifi_secrets.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_WIFI_RETRY     10

#define MAX_RUNTIME_ALARMS          10
#define CAMERA_PAYLOAD_LEN          256
#define DEMO_ALARM_DELAY_SECONDS    60
#define DEMO_PROMPT_TIMEOUT_SECONDS 15
#define TIME_LOG_PERIOD_SECONDS     5

#define BOOT_BUTTON_GPIO GPIO_NUM_0

static const char *TAG = "ESP32_PRINCIPAL";

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count = 0;

typedef struct {
    alarma_config_t config;
    int last_yday;
    int last_hour;
    int last_minute;
} alarma_runtime_t;

static alarma_runtime_t alarmas_runtime[MAX_RUNTIME_ALARMS];
static int num_alarmas_runtime = 0;

static const char *nombre_dia(int weekday)
{
    switch (weekday) {
    case 0: return "domingo";
    case 1: return "lunes";
    case 2: return "martes";
    case 3: return "miercoles";
    case 4: return "jueves";
    case 5: return "viernes";
    case 6: return "sabado";
    default: return "desconocido";
    }
}

static const char *dificultad_a_texto(dificultad_t difficulty)
{
    switch (difficulty) {
    case DIFICULTAD_BAJA:  return "baja";
    case DIFICULTAD_MEDIA: return "media";
    case DIFICULTAD_ALTA:  return "alta";
    default:               return "desconocida";
    }
}

static const char *gesto_a_texto(gesto_t gesto)
{
    switch (gesto) {
    case GESTO_IZQUIERDA: return "izquierda";
    case GESTO_DERECHA:   return "derecha";
    case GESTO_ARRIBA:    return "arriba";
    case GESTO_ABAJO:     return "abajo";
    case GESTO_CERCA:     return "cerca";
    case GESTO_LEJOS:     return "lejos";
    default:              return "desconocido";
    }
}

static int cantidad_gestos_por_dificultad(dificultad_t difficulty)
{
    switch (difficulty) {
    case DIFICULTAD_BAJA:  return 3;
    case DIFICULTAD_MEDIA: return 5;
    case DIFICULTAD_ALTA:  return 7;
    default:               return 3;
    }
}

static void inicializar_estado_alarma(alarma_runtime_t *alarm)
{
    alarm->last_yday = -1;
    alarm->last_hour = -1;
    alarm->last_minute = -1;
}

static void cargar_alarmas_configuradas(void)
{
    num_alarmas_runtime = 0;

    int alarmas_a_cargar = num_alarmas_config;

    if (alarmas_a_cargar > MAX_RUNTIME_ALARMS) {
        alarmas_a_cargar = MAX_RUNTIME_ALARMS;
        ESP_LOGW(TAG,
                 "Hay mas alarmas configuradas que espacio disponible. "
                 "Solo se cargaran %d.",
                 MAX_RUNTIME_ALARMS);
    }

    for (int i = 0; i < alarmas_a_cargar; i++) {
        alarmas_runtime[num_alarmas_runtime].config = alarmas_config[i];
        inicializar_estado_alarma(&alarmas_runtime[num_alarmas_runtime]);
        num_alarmas_runtime++;
    }
}

static void imprimir_alarmas_configuradas(void)
{
    ESP_LOGI(TAG, "Alarmas cargadas: %d", num_alarmas_runtime);

    for (int i = 0; i < num_alarmas_runtime; i++) {
        const alarma_config_t *alarm = &alarmas_runtime[i].config;

        ESP_LOGI(TAG,
                 "[%d] id=%d | %s | %s %02d:%02d | dificultad=%s | %s",
                 i,
                 alarm->id,
                 alarm->enabled ? "activa" : "inactiva",
                 nombre_dia(alarm->weekday),
                 alarm->hour,
                 alarm->minute,
                 dificultad_a_texto(alarm->difficulty),
                 alarm->name);
    }
}

static void generar_secuencia(secuencia_t *sequence, dificultad_t difficulty)
{
    sequence->length = cantidad_gestos_por_dificultad(difficulty);

    if (sequence->length > MAX_SEQUENCE_LEN) {
        sequence->length = MAX_SEQUENCE_LEN;
    }

    for (int i = 0; i < sequence->length; i++) {
        sequence->gestos[i] = (gesto_t)(esp_random() % NUM_GESTOS);
    }
}

static void imprimir_secuencia(const secuencia_t *sequence)
{
    ESP_LOGI(TAG, "Secuencia solicitada al usuario:");
    vTaskDelay(pdMS_TO_TICKS(100));

    for (int i = 0; i < sequence->length; i++) {
        ESP_LOGI(TAG,
                 "Gesto %d/%d: %s",
                 i + 1,
                 sequence->length,
                 gesto_a_texto(sequence->gestos[i]));

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void payload_append(char *out,
                           size_t out_len,
                           size_t *used,
                           const char *format,
                           ...)
{
    if (*used >= out_len) {
        return;
    }

    va_list args;
    va_start(args, format);

    int written = vsnprintf(out + *used,
                            out_len - *used,
                            format,
                            args);

    va_end(args);

    if (written < 0) {
        return;
    }

    if ((size_t)written >= out_len - *used) {
        *used = out_len - 1;
    } else {
        *used += (size_t)written;
    }
}

static void construir_payload_esp32cam(const alarma_config_t *alarm,
                                       const secuencia_t *sequence,
                                       char *out,
                                       size_t out_len)
{
    size_t used = 0;

    if (out_len == 0) {
        return;
    }

    out[0] = '\0';

    payload_append(out,
                   out_len,
                   &used,
                   "{\"cmd\":\"start_alarm\","
                   "\"alarm_id\":%d,"
                   "\"difficulty\":\"%s\","
                   "\"sequence\":[",
                   alarm->id,
                   dificultad_a_texto(alarm->difficulty));

    for (int i = 0; i < sequence->length; i++) {
        if (i > 0) {
            payload_append(out, out_len, &used, ",");
        }

        payload_append(out,
                       out_len,
                       &used,
                       "\"%s\"",
                       gesto_a_texto(sequence->gestos[i]));
    }

    payload_append(out, out_len, &used, "],\"timeout_s\":60}");
}

static void preparar_solicitud_para_esp32cam(const alarma_config_t *alarm,
                                             const secuencia_t *sequence)
{
    char payload[CAMERA_PAYLOAD_LEN];

    construir_payload_esp32cam(alarm,
                               sequence,
                               payload,
                               sizeof(payload));

    ESP_LOGI(TAG, "Payload listo para ESP32-CAM:");
    ESP_LOGI(TAG, "%s", payload);

    /*
     * Mas adelante esta funcion enviara el payload por WiFi
     * a la ESP32-CAM. Por ahora se imprime para verificar
     * el formato del mensaje.
     */
}

static bool alarma_ya_fue_disparada(const alarma_runtime_t *alarm,
                                    const struct tm *timeinfo)
{
    return (alarm->last_yday == timeinfo->tm_yday &&
            alarm->last_hour == timeinfo->tm_hour &&
            alarm->last_minute == timeinfo->tm_min);
}

static void registrar_alarma_disparada(alarma_runtime_t *alarm,
                                       const struct tm *timeinfo)
{
    alarm->last_yday = timeinfo->tm_yday;
    alarm->last_hour = timeinfo->tm_hour;
    alarm->last_minute = timeinfo->tm_min;
}

static void disparar_alarma(alarma_runtime_t *alarm,
                            const struct tm *timeinfo)
{
    secuencia_t sequence;

    registrar_alarma_disparada(alarm, timeinfo);
    generar_secuencia(&sequence, alarm->config.difficulty);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ALARMA ACTIVADA");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Nombre: %s", alarm->config.name);
    ESP_LOGI(TAG, "ID: %d", alarm->config.id);
    ESP_LOGI(TAG, "Dia: %s", nombre_dia(alarm->config.weekday));
    ESP_LOGI(TAG,
             "Hora configurada: %02d:%02d",
             alarm->config.hour,
             alarm->config.minute);
    ESP_LOGI(TAG,
             "Dificultad: %s",
             dificultad_a_texto(alarm->config.difficulty));

    vTaskDelay(pdMS_TO_TICKS(100));

    imprimir_secuencia(&sequence);

    vTaskDelay(pdMS_TO_TICKS(100));

    preparar_solicitud_para_esp32cam(&alarm->config, &sequence);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");
}

static void revisar_alarmas(const struct tm *timeinfo)
{
    for (int i = 0; i < num_alarmas_runtime; i++) {
        alarma_runtime_t *alarm = &alarmas_runtime[i];

        if (!alarm->config.enabled) {
            continue;
        }

        bool same_day = (alarm->config.weekday == timeinfo->tm_wday);
        bool same_hour = (alarm->config.hour == timeinfo->tm_hour);
        bool same_minute = (alarm->config.minute == timeinfo->tm_min);

        if (!(same_day && same_hour && same_minute)) {
            continue;
        }

        if (alarma_ya_fue_disparada(alarm, timeinfo)) {
            continue;
        }

        disparar_alarma(alarm, timeinfo);
        return;
    }
}

static bool agregar_alarma_demo(int seconds_from_now)
{
    if (num_alarmas_runtime >= MAX_RUNTIME_ALARMS) {
        ESP_LOGW(TAG, "No hay espacio para agregar la alarma demo");
        return false;
    }

    time_t now;
    struct tm demo_time;

    time(&now);
    now += seconds_from_now;
    localtime_r(&now, &demo_time);

    alarma_runtime_t *demo = &alarmas_runtime[num_alarmas_runtime];

    demo->config.id = 900;
    demo->config.enabled = true;
    demo->config.weekday = demo_time.tm_wday;
    demo->config.hour = demo_time.tm_hour;
    demo->config.minute = demo_time.tm_min;
    demo->config.difficulty = DIFICULTAD_BAJA;

    snprintf(demo->config.name,
             sizeof(demo->config.name),
             "Alarma demo temporal");

    inicializar_estado_alarma(demo);

    num_alarmas_runtime++;

    ESP_LOGI(TAG,
             "Alarma demo agregada para %s %02d:%02d",
             nombre_dia(demo->config.weekday),
             demo->config.hour,
             demo->config.minute);

    return true;
}

static void configurar_boton_boot(void)
{
    gpio_reset_pin(BOOT_BUTTON_GPIO);
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);
}

static bool boot_button_pressed(void)
{
    return gpio_get_level(BOOT_BUTTON_GPIO) == 0;
}

static bool configurar_stdin_no_bloqueante(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);

    if (flags < 0) {
        ESP_LOGW(TAG, "No se pudo configurar entrada por monitor serial");
        return false;
    }

    if (fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGW(TAG, "No se pudo activar entrada no bloqueante");
        return false;
    }

    return true;
}

static bool preguntar_si_agregar_alarma_demo(void)
{
    bool stdin_ok = configurar_stdin_no_bloqueante();

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "Desea crear una alarma de prueba a 1 minuto?");
    ESP_LOGI(TAG, "Escriba 's' y Enter para SI, o 'n' y Enter para NO.");
    ESP_LOGI(TAG, "Tambien puede presionar el boton BOOT para crearla.");
    ESP_LOGI(TAG,
             "Si no responde en %d segundos, se continua sin alarma demo.",
             DEMO_PROMPT_TIMEOUT_SECONDS);
    ESP_LOGI(TAG, "");

    int64_t start_us = esp_timer_get_time();
    int last_seconds_left = -1;

    while (true) {
        int64_t elapsed_us = esp_timer_get_time() - start_us;

        if (elapsed_us >= DEMO_PROMPT_TIMEOUT_SECONDS * 1000000LL) {
            ESP_LOGI(TAG, "No se creo alarma demo");
            return false;
        }

        int seconds_left =
            DEMO_PROMPT_TIMEOUT_SECONDS - (int)(elapsed_us / 1000000LL);

        if (seconds_left != last_seconds_left &&
            (seconds_left == 10 ||
             seconds_left == 5 ||
             seconds_left <= 3)) {
            ESP_LOGI(TAG, "Esperando respuesta... %d s", seconds_left);
            last_seconds_left = seconds_left;
        }

        if (stdin_ok) {
            int c = getchar();

            if (c == 's' || c == 'S') {
                ESP_LOGI(TAG, "Respuesta recibida: SI");
                return true;
            }

            if (c == 'n' || c == 'N') {
                ESP_LOGI(TAG, "Respuesta recibida: NO");
                return false;
            }

            if (c == EOF) {
                clearerr(stdin);
            }
        }

        if (boot_button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(50));

            if (boot_button_pressed()) {
                ESP_LOGI(TAG, "Boton BOOT presionado. Se crea alarma demo.");
                return true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void imprimir_hora_actual(const struct tm *timeinfo)
{
    ESP_LOGI(TAG,
             "Hora actual Costa Rica: %s %02d/%02d/%04d %02d:%02d:%02d",
             nombre_dia(timeinfo->tm_wday),
             timeinfo->tm_mday,
             timeinfo->tm_mon + 1,
             timeinfo->tm_year + 1900,
             timeinfo->tm_hour,
             timeinfo->tm_min,
             timeinfo->tm_sec);
}

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

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
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

        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No se pudo crear el grupo de eventos WiFi");
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

    ESP_LOGI(TAG, "Configuracion WiFi lista. Esperando conexion...");

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Conectado a la red WiFi: %s", WIFI_SSID);
        return true;
    }

    ESP_LOGE(TAG, "No se pudo conectar a la red WiFi: %s", WIFI_SSID);
    return false;
}

static bool sincronizar_hora_ntp(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");

    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    esp_netif_sntp_init(&sntp_config);

    ESP_LOGI(TAG, "Esperando sincronizacion por NTP...");

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo sincronizar hora por NTP");
        return false;
    }

    setenv("TZ", "CST6", 1);
    tzset();

    ESP_LOGI(TAG, "Hora sincronizada correctamente");
    return true;
}

static void detener_por_error(void)
{
    ESP_LOGE(TAG, "El sistema no puede continuar sin WiFi y hora valida");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Nodo principal iniciado");

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

    configurar_boton_boot();

    if (!wifi_connect()) {
        detener_por_error();
    }

    if (!sincronizar_hora_ntp()) {
        detener_por_error();
    }

    cargar_alarmas_configuradas();
    imprimir_alarmas_configuradas();

    if (preguntar_si_agregar_alarma_demo()) {
    agregar_alarma_demo(DEMO_ALARM_DELAY_SECONDS);
    }

    int last_logged_second = -1;

    while (true) {
        time_t now;
        struct tm timeinfo;

        time(&now);
        localtime_r(&now, &timeinfo);

        if ((timeinfo.tm_sec % TIME_LOG_PERIOD_SECONDS) == 0 &&
            timeinfo.tm_sec != last_logged_second) {
            imprimir_hora_actual(&timeinfo);
            last_logged_second = timeinfo.tm_sec;
        }

        revisar_alarmas(&timeinfo);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
