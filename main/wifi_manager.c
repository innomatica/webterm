#include <string.h>
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "wifi_manager.h"

#if CONFIG_WEBTERM_WIFI_AUTH_OPEN
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_WEBTERM_WIFI_AUTH_WEP
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA2_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA_WPA2_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA2_ENTERPRISE
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_ENTERPRISE
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA3_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_WEBTERM_WIFI_AUTH_WPA2_WPA3_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_WEBTERM_WIFI_AUTH_WAPI_PSK
	#define WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static const char *TAG = "wifi_manager";

static bool is_our_netif(const char *prefix, esp_netif_t *netif);
static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void on_wifi_connect(void *esp_netif, esp_event_base_t event_base,
                            int32_t event_id, void *event_data);
static void on_sta_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data);
static void wifi_start(void);
static void wifi_stop(void);
static esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config, bool wait);
static esp_err_t wifi_sta_do_disconnect(void);

static esp_netif_t *s_uartbrg_sta_netif = NULL;
static SemaphoreHandle_t s_semph_get_ip_addrs = NULL;
static int s_retry_num = 0;


static bool is_our_netif(const char *prefix, esp_netif_t *netif)
{
    return strncmp(prefix, esp_netif_get_desc(netif), strlen(prefix) - 1) == 0;
}

static void on_wifi_disconnect(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    s_retry_num++;
    if (s_retry_num > WIFI_CONN_MAX_RETRY) {
        ESP_LOGI(TAG, "WiFi Connect failed %d times, stop reconnect.", s_retry_num);
        /* let wifi_sta_do_connect() return */
        if (s_semph_get_ip_addrs) {
            xSemaphoreGive(s_semph_get_ip_addrs);
        }
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        return;
    }
    ESP_ERROR_CHECK(err);
}

static void on_wifi_connect(void *esp_netif, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
}

static void on_sta_got_ip(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    s_retry_num = 0;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (!is_our_netif(WEBTERM_NETIF_DESC_STA, event->esp_netif)) {
        return;
    }
    ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR,
			esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
    if (s_semph_get_ip_addrs) {
        xSemaphoreGive(s_semph_get_ip_addrs);
    } else {
        ESP_LOGI(TAG, "- IPv4 address: " IPSTR ",", IP2STR(&event->ip_info.ip));
    }
}


static void wifi_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    // Warning: the interface desc is used in tests to capture actual connection details (IP, gw, mask)
    esp_netif_config.if_desc = WEBTERM_NETIF_DESC_STA;
    esp_netif_config.route_prio = 128;
    s_uartbrg_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);
    esp_wifi_set_default_wifi_sta_handlers();

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}


static void wifi_stop(void)
{
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        return;
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(esp_wifi_clear_default_wifi_driver_and_handlers(s_uartbrg_sta_netif));
    esp_netif_destroy(s_uartbrg_sta_netif);
    s_uartbrg_sta_netif = NULL;
}


static esp_err_t wifi_sta_do_connect(wifi_config_t wifi_config, bool wait)
{
    if (wait) {
        s_semph_get_ip_addrs = xSemaphoreCreateBinary();
        if (s_semph_get_ip_addrs == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
				WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
				IP_EVENT_STA_GOT_IP, &on_sta_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
				WIFI_EVENT_STA_CONNECTED, &on_wifi_connect, s_uartbrg_sta_netif));

    ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed! ret:%x", ret);
        return ret;
    }
    if (wait) {
        ESP_LOGI(TAG, "Waiting for IP(s)");
        xSemaphoreTake(s_semph_get_ip_addrs, portMAX_DELAY);
        if (s_retry_num > WIFI_CONN_MAX_RETRY) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t wifi_sta_do_disconnect(void)
{
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT,
				WIFI_EVENT_STA_DISCONNECTED, &on_wifi_disconnect));
    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT,
				IP_EVENT_STA_GOT_IP, &on_sta_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT,
				WIFI_EVENT_STA_CONNECTED, &on_wifi_connect));
    if (s_semph_get_ip_addrs) {
        vSemaphoreDelete(s_semph_get_ip_addrs);
    }
    return esp_wifi_disconnect();
}

void wifi_shutdown(void)
{
    wifi_sta_do_disconnect();
    wifi_stop();
}

esp_err_t wifi_connect(void)
{
    ESP_LOGI(TAG, "Start connect.");
    wifi_start();
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WEBTERM_WIFI_SSID,
            .password = CONFIG_WEBTERM_WIFI_PASSWORD,
            .scan_method = WIFI_SCAN_METHOD,
            .sort_method = WIFI_CONNECT_AP_SORT_METHOD,
            .threshold.rssi = WIFI_SCAN_RSSI_THRESHOLD,
            .threshold.authmode = WEBTERM_WIFI_SCAN_AUTH_MODE_THRESHOLD,
        },
    };
    return wifi_sta_do_connect(wifi_config, true);
}

