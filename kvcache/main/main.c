#include <stdio.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "kv.h"
#include "server.h"
#include "sysmon.h"
#include "wifi_creds.h"

static const char *TAG = "kvcache";
static EventGroupHandle_t wev;
#define GOT_IP BIT0

void engine_bench_run(size_t reserve);
void mc_bench_run(size_t reserve);
void loopback_run(void);
void engine_test_run(size_t reserve);
void p3_bench_run(size_t reserve);

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wev, GOT_IP);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        printf("IP " IPSTR "\n", IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(wev, GOT_IP);
    }
}

static void wifi_up(void)
{
    wev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE); /* same as MicroPython run: no modem sleep */
    xEventGroupWaitBits(wev, GOT_IP, pdFALSE, pdTRUE, portMAX_DELAY);
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    size_t reserve = CONFIG_KV_HEAP_RESERVE_KB * 1024;
    printf("VARIANT engine=%s server=%s cores=%s iram=%d\n", kv_engine_name(), variant_server_name(),
           variant_cores_name(), KV_IRAM_ON);
#if CONFIG_KV_ENGINE_TEST
    engine_test_run(reserve);
#endif
#if CONFIG_KV_MC_BENCH
    mc_bench_run(reserve);
#elif CONFIG_KV_ENGINE_SELFTEST
    engine_bench_run(reserve);
#endif
#if CONFIG_KV_P3_BENCH
    p3_bench_run(reserve);
#endif
#if CONFIG_KV_NO_WIFI && CONFIG_KV_LOOPBACK_TEST
    /* real server, no radio: lwIP + loopback interface, clients on the board */
    ESP_ERROR_CHECK(esp_netif_init());
    if (kv_init(reserve) != 0) ESP_LOGE(TAG, "kv_init failed");
    server_start();
    vTaskDelay(pdMS_TO_TICKS(200));
    printf("VARIANT engine=%s server=%s cores=%s\n", kv_engine_name(), variant_server_name(), variant_cores_name());
    loopback_run();
    printf("DONE (loopback build, WiFi disabled)\n");
    return;
#elif CONFIG_KV_NO_WIFI
    printf("DONE (engine-only build, WiFi disabled)\n");
    return;
#endif
    wifi_up();
    if (kv_init(reserve) != 0) ESP_LOGE(TAG, "kv_init failed");
    server_start();
    printf("READY heap_free=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}
