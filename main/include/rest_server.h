#ifndef REST_SERVER_H_
#define REST_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

#define FILE_PATH_MAX		(ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE		(10240)

// TODO: bring this to kconfig
#define UART_BUF_SIZE		(1024)
#define UART_PORT_NUM		UART_NUM_1	// UART_NUM_0 is used by the DevKit USB

#define GPIO_PWR_WAKE		(16)	// Set ground to shutdown
									//  PIN 5: dtoverlay=gpio-shutdown
									//  PIN X: dtoverlay=gpio-shutdown,gpio-pin=X
#define GPIO_PWR_SHDN		(16)	// RPi PIN 5: Set ground to wake up
#define GPIO_UART_TXD		(14)	// RPi Pin 10
#define GPIO_UART_RXD		(13)	// RPi Pin 8

esp_err_t start_rest_server(const char *base_path);


#ifdef __cplusplus
}
#endif

#endif // REST_SERVER_H_
