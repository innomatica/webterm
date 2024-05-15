#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#ifdef __cplusplus
extern "C" {
#endif

#define WEBTERM_NETIF_DESC_STA					"webterm_netif_sta"
//#define WIFI_SCAN_METHOD						WIFI_ALL_CHANNEL_SCAN
#define WIFI_SCAN_METHOD						WIFI_FAST_SCAN
#define WIFI_CONNECT_AP_SORT_METHOD				WIFI_CONNECT_AP_BY_SIGNAL
#define WIFI_SCAN_RSSI_THRESHOLD				(-127)
#define WIFI_CONN_MAX_RETRY						(6)

esp_err_t wifi_connect(void);
void wifi_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H_
