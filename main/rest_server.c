#include <string.h>
#include <fcntl.h>
#include "esp_http_server.h"
#include "esp_chip_info.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
//#include "freertos/semphr.h"

#include "rest_server.h"

static const char *TAG = "rest_server";

#define ECHO_TEST					(0)	// echo back test for websocket
#define USE_STREAM_CALLBACK			(0)	// untested: try only when necessary
#define REST_CHECK(a, str, goto_tag, ...) \
    do { \
		if (!(a)) { \
			ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
		goto goto_tag; \
		} \
	} while (0)
#define CHECK_FILE_EXTENSION(filename, ext) \
	(strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

static esp_err_t init_hardware(void);
static void ws_async_send(void *arg);
static void target_pwr_ctrl_task(void *pvParameters);
static void uart_read_task(void *pvParameters);
static esp_err_t set_content_type_from_file(httpd_req_t *req,
		const char *filepath);
/* REST endpoint handlers */
static esp_err_t rest_common_get_handler(httpd_req_t *req);
static esp_err_t power_post_handler(httpd_req_t *req);
static esp_err_t power_get_handler(httpd_req_t *req);
static esp_err_t websocket_handler(httpd_req_t *req);

#if USE_STREAM_CALLBACK
void vStreamSendCallback(StreamBufferHandle_t xStreamBuffer,
		BaseType_t xIsInsideISR, BaseType_t * const pxPriority);
#endif

/* rest server context data structure */
typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

/* response data structure */
typedef struct _resp_data {
    httpd_handle_t hd;
    int fd;
	uint8_t data[UART_BUF_SIZE];
} resp_data_t;

resp_data_t resp_arg;				// response data
uint8_t uart_data[UART_BUF_SIZE];	// uart data
StreamBufferHandle_t xDataBuffer;	// stream buffer from UART to WS



/*
 * initialize UART port and power state monitor port
 */
static esp_err_t init_hardware() {
	// UART
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
	ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, GPIO_UART_TXD, GPIO_UART_RXD,
				UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
	ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE,
				UART_BUF_SIZE, 0, NULL, 0));
	return ESP_OK;
}


/*
 * power control task
 */
static void target_pwr_ctrl_task(void *pvParameters) {
	int gpio_pin;
	gpio_config_t io_config = {};

	if((int)pvParameters == 1) {
		// wake up
		gpio_pin = GPIO_PWR_WAKE;
		ESP_LOGI(TAG, "waking up target");
	} else {
		// shutdown
		gpio_pin = GPIO_PWR_SHDN;
		ESP_LOGI(TAG, "shutting down target");
	}

	// set up the control pin
	io_config.intr_type = GPIO_INTR_DISABLE;
	io_config.mode = GPIO_MODE_OUTPUT;
	io_config.pin_bit_mask = (1ULL << gpio_pin);
	io_config.pull_down_en = 0;
	io_config.pull_up_en = 1;	// prevent glitch ?
	gpio_config(&io_config);

	// 500 msec active low pulse
	gpio_set_level(gpio_pin, 0);
	vTaskDelay( 500 / portTICK_PERIOD_MS);
	gpio_set_level(gpio_pin, 1);

	// reset the pin
	gpio_reset_pin(gpio_pin);

	// kill itself
	vTaskDelete(NULL);
}

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void *arg)
{
    resp_data_t *r = arg;

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

	httpd_handle_t hd = r->hd;
	int fd = r->fd;

	int len = xStreamBufferReceive(xDataBuffer, r->data, sizeof(r->data),
			(TickType_t) 10);
	ws_pkt.len = len;
	ws_pkt.payload = r->data;
	ESP_LOGD(TAG, "From Buffer: %.*s", len, ws_pkt.payload);
	httpd_ws_send_frame_async(hd, fd, &ws_pkt);
}


/*
 * UART incoming data handling
 * FIXME: being a task, this could be turned into general UART event handler
 */
static void uart_read_task(void *pvParameters) {
    httpd_ws_frame_t ws_pkt;
	memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
	ws_pkt.type = HTTPD_WS_TYPE_TEXT;

	while(1) {
		// wait for UART RX input
		int len = uart_read_bytes(UART_PORT_NUM, uart_data, (UART_BUF_SIZE -1),
				20 / portTICK_PERIOD_MS);
		// need to terminate the string
		uart_data[len] = 0x00;

		if(len) {
			ESP_LOGD(TAG, "From UART: %.*s", len, uart_data);
			// push data into stream
			xStreamBufferSend(xDataBuffer, uart_data, len, (TickType_t) 10);
#if USE_STREAM_CALLBACK
#else
			// send data using httpd_queue_work
			esp_err_t ret = httpd_queue_work(resp_arg.hd, ws_async_send,
					(void *)&resp_arg);
			if(ret != ESP_OK) {
				ESP_LOGE(TAG, "httpd_queue_work failed");
			}
#endif
		}
	}
}

#if USE_STREAM_CALLBACK
/*
 * callback version of stream handling
 * WARNING: not tested
 */
void vStreamSendCallback(StreamBufferHandle_t xStreamBuffer,
		BaseType_t xIsInsideISR, BaseType_t * const pxPriority) {
	ESP_LOGD(TAG, "vStreamSendCallback:");
	// send data using httpd_queue_work
	esp_err_t ret = httpd_queue_work(resp_arg.hd, ws_async_send, (void *)&resp_arg);
	if(ret != ESP_OK) {
		ESP_LOGE(TAG, "httpd_queue_work failed");
	}
}
#endif

/*
 * Set HTTP response content type according to file extension
 * https://www.iana.org/assignments/media-types/media-types.xhtml
 */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "text/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "image/svg+xml";
    }
    return httpd_resp_set_type(req, type);
}

/*
 * Send HTTP response with the contents of the requested file
 */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
						"Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/*
 * handler: POST power control
 */
static esp_err_t power_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    char *buf = ((rest_server_context_t *)(req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
				"content too long");
        return ESP_FAIL;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
					"Failed to post control value");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    char *state = cJSON_GetObjectItem(root, "power")->valuestring;

	if( strncmp((const char*)state, "on", sizeof(state)) == 0 ||
			strncmp((const char*)state, "ON", sizeof(state)) == 0 ||
			strncmp((const char*)state, "On", sizeof(state)) == 0) {
		httpd_resp_sendstr(req, "Waking up target device");
		//target_pwr_ctrl(1);
		xTaskCreate(target_pwr_ctrl_task, "target_pwr_ctrl_task", 2048,
				(void*)1, 10, NULL);
	} else if( strncmp((const char*)state, "off", sizeof(state)) == 0 ||
			strncmp((const char*)state, "Off", sizeof(state)) == 0 ||
			strncmp((const char*)state, "Off", sizeof(state)) == 0) {
		httpd_resp_sendstr(req, "Shutting down target device");
		//target_pwr_ctrl(0);
		xTaskCreate(target_pwr_ctrl_task, "target_pwr_ctrl_task", 2048,
				(void*)0, 10, NULL);
	} else {
		httpd_resp_sendstr(req, "Invalid power state detected");
	}

    ESP_LOGI(TAG, "Power control: %s", state);

    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * handler: GET power state
 */
static esp_err_t power_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    cJSON *root = cJSON_CreateObject();

	// set RXD port to PULLDOWN mode
	gpio_set_pull_mode(GPIO_UART_RXD, GPIO_PULLDOWN_ONLY);
	// read power state by reading RXD port
	int state = gpio_get_level(GPIO_UART_RXD);
	// set the mode back
	gpio_set_pull_mode(GPIO_UART_RXD, GPIO_PULLUP_ONLY);
	// WARNING: you may get a garbage char like 0x00 due to the RXD disruption

	// build json data
	if(state == 1) {
		cJSON_AddStringToObject(root, "power", "on");
	} else {
		cJSON_AddStringToObject(root, "power", "off");
	}
    const char *power_state = cJSON_Print(root);
    ESP_LOGI(TAG, "Power state: %s", power_state);

	// send data
    httpd_resp_sendstr(req, power_state);
    free((void *)power_state);
    cJSON_Delete(root);

    return ESP_OK;
}

/*
 * handler: websocket
 */
static esp_err_t websocket_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
		// ws connection request
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
		// set resp_arg params
		resp_arg.hd = req->handle;
		resp_arg.fd = httpd_req_to_sockfd(req);

        return ESP_OK;
    }

	// incoming ws packet handled here
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    ESP_LOGD(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGD(TAG, "Got packet with message: %s", ws_pkt.payload);
    }

    ESP_LOGD(TAG, "Packet type: %d", ws_pkt.type);
	/*
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char*)ws_pkt.payload,"Trigger async") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }
	*/

#if ECHO_TEST
    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }
#else
	// send data to UART
	uart_write_bytes(UART_PORT_NUM, (const char *)ws_pkt.payload, ws_pkt.len);

#endif

    free(buf);
    return ret;
}

/*
 * start server
 */
esp_err_t start_rest_server(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
	ESP_ERROR_CHECK(init_hardware());

	// create a stream buffer
#if USE_STREAM_CALLBACK
	xDataBuffer = xStreamBufferCreateWithCallback(UART_BUF_SIZE * 2, 1,
			vStreamSendCallback, NULL);
#else
	xDataBuffer = xStreamBufferCreate(UART_BUF_SIZE * 2, 1);
#endif

    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed",
			err_start);

    // URI handler for power control
    httpd_uri_t power_post_uri = {
        .uri = "/api/v1/pwrctrl",
        .method = HTTP_POST,
        .handler = power_post_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &power_post_uri);

    // URI handler for power status
    httpd_uri_t power_get_uri = {
        .uri = "/api/v1/pwrstate",
        .method = HTTP_GET,
        .handler = power_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &power_get_uri);

	// URI hander for websocket
    httpd_uri_t websocket_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .user_ctx = rest_context,
		.is_websocket = true
    };
    httpd_register_uri_handler(server, &websocket_uri);

    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

	// create uart incoming task
	xTaskCreate(uart_read_task, "uart_read_task", UART_BUF_SIZE + 1024,
			NULL, 10, NULL);

    return ESP_OK;

err_start:
    free(rest_context);

err:
    return ESP_FAIL;
}

