#include "wifi_screen.h"
#include "jpeg_decoder.h"
#include "lv_port.h"
#include "ft6336u_driver.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>

#define TAG "wifi_screen"

#define SCR_W       320
#define SCR_H       240
#define JPEG_MAX    (128 * 1024)
#define TCP_PORT    8080
#define TOUCH_PORT  8082
#define HTTP_PORT   80
#define AP_SSID     "ESP_Screen"
#define NVS_NS      "wifi_cfg"

#define SYNC_0      0xAA
#define SYNC_1      0x55
enum { ST_SYNC0, ST_SYNC1, ST_SIZE, ST_DATA };

static EventGroupHandle_t s_wifi_event;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static int s_retry = 0;
#define MAX_RETRY 10

/* ── NVS helpers ─────────────────────────────── */

static bool nvs_load_credentials(char *ssid, char *pass, size_t ssid_len, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t r1 = nvs_get_str(h, "ssid", ssid, &ssid_len);
    esp_err_t r2 = nvs_get_str(h, "pass", pass, &pass_len);
    nvs_close(h);
    return (r1 == ESP_OK && r2 == ESP_OK && strlen(ssid) > 0);
}

static void nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
}

void wifi_screen_clear_config(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "WiFi config cleared, will enter AP mode on next boot");
}

/* ── WiFi event handler ──────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_retry < MAX_RETRY) {
                esp_wifi_connect();
                s_retry++;
                ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", s_retry, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event, WIFI_FAIL_BIT);
            }
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected! IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
    }
}

/* ── STA mode: connect to router ─────────────── */

static bool wifi_sta_start(const char *ssid, const char *pass)
{
    s_wifi_event = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t inst_any, inst_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &inst_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(8);  /* 8 = 2dBm, lowest */

    ESP_LOGI(TAG, "Connecting to \"%s\"...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    ESP_LOGW(TAG, "Failed to connect to \"%s\"", ssid);
    return false;
}

/* ── AP mode web config page ─────────────────── */

static const char HTML_PAGE[] =
"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ESP Screen WiFi</title>"
"<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}"
"input{width:100%;padding:8px;margin:4px 0 12px;box-sizing:border-box}"
"button{background:#4CAF50;color:#fff;border:none;padding:12px 24px;font-size:16px;"
"border-radius:4px;cursor:pointer;width:100%}button:hover{background:#45a049}"
"h2{color:#333}</style></head><body>"
"<h2>WiFi Screen Config</h2>"
"<form method='POST' action='/save'>"
"<label>WiFi SSID</label><input name='ssid' required>"
"<label>WiFi Password</label><input name='pass' type='password'>"
"<button type='submit'>Save & Reboot</button></form></body></html>";

static const char HTML_SAVED[] =
"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
"<!DOCTYPE html><html><body style='text-align:center;margin-top:80px;font-family:sans-serif'>"
"<h2>Saved! Rebooting...</h2>"
"<p>Connect your PC to the same WiFi network, then run wifi_mirror.py</p>"
"</body></html>";

static void url_decode(char *dst, const char *src, size_t dst_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_len - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static void http_server_task(void *arg)
{
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "HTTP socket failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(HTTP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "HTTP bind failed");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }
    listen(server_fd, 1);
    ESP_LOGI(TAG, "Config web server on http://192.168.4.1");

    while (1) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int fd = accept(server_fd, (struct sockaddr *)&client, &len);
        if (fd < 0) continue;

        char buf[1024];
        int n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(fd); continue; }
        buf[n] = '\0';

        if (strncmp(buf, "POST /save", 10) == 0) {
            /* Find body after \r\n\r\n */
            char *body = strstr(buf, "\r\n\r\n");
            if (body) {
                body += 4;
                char ssid[33] = {0}, pass[65] = {0};
                /* Parse ssid=...&pass=... */
                char *p = strstr(body, "ssid=");
                if (p) {
                    p += 5;
                    char *end = strchr(p, '&');
                    if (end) {
                        char tmp[64] = {0};
                        size_t cp = (end - p < 63) ? end - p : 63;
                        strncpy(tmp, p, cp);
                        url_decode(ssid, tmp, sizeof(ssid));
                    }
                }
                p = strstr(body, "pass=");
                if (p) {
                    p += 5;
                    char tmp[128] = {0};
                    size_t cp = strlen(p);
                    if (cp > 127) cp = 127;
                    strncpy(tmp, p, cp);
                    url_decode(pass, tmp, sizeof(pass));
                }

                if (strlen(ssid) > 0) {
                    nvs_save_credentials(ssid, pass);
                    send(fd, HTML_SAVED, strlen(HTML_SAVED), 0);
                    close(fd);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_restart();
                    return;
                }
            }
        }
        send(fd, HTML_PAGE, strlen(HTML_PAGE), 0);
        close(fd);
    }
}

static void wifi_ap_start(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 1,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(AP_SSID);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(8);  /* 8 = 2dBm, lowest */

    ESP_LOGI(TAG, "AP mode started: SSID=\"%s\"", AP_SSID);
    ESP_LOGI(TAG, "Connect and open http://192.168.4.1 to configure WiFi");

    xTaskCreate(http_server_task, "http_srv", 4096, NULL, 3, NULL);
}

/* ── Touch sender task ───────────────────────── */

static volatile int s_touch_fd = -1;
static volatile bool s_touch_running = false;

static void touch_task(void *arg)
{
    uint32_t pc_ip = (uint32_t)(uintptr_t)arg;

    ft6336u_driver(GPIO_NUM_13, GPIO_NUM_12);
    ft6336u_int_info(true);
    ESP_LOGI(TAG, "FT6336U touch initialized");

    /* Connect to PC touch server */
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "Touch socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TOUCH_PORT),
        .sin_addr.s_addr = pc_ip,
    };

    ESP_LOGI(TAG, "Connecting to PC touch port %d...", TOUCH_PORT);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Touch connect failed");
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Touch channel connected");

    s_touch_fd = fd;
    s_touch_running = true;

    int16_t x, y;
    int state, last_state = 0;

    while (s_touch_running) {
        ft6336u_read(&x, &y, &state);

        if (state != last_state || state == 1) {
            /* Swap X/Y and mirror X to match screen orientation */
            int16_t tx = SCR_W - 1 - y;
            int16_t ty = x;
            uint8_t pkt[6] = {
                0xBB,
                (uint8_t)state,
                (uint8_t)(tx >> 8), (uint8_t)(tx & 0xFF),
                (uint8_t)(ty >> 8), (uint8_t)(ty & 0xFF),
            };
            if (send(fd, pkt, 6, 0) <= 0) {
                ESP_LOGI(TAG, "Touch send failed, PC disconnected");
                break;
            }
            last_state = state;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    close(fd);
    s_touch_fd = -1;
    vTaskDelete(NULL);
}

/* ── TCP frame receiver server ───────────────── */

static void tcp_frame_task(void *arg)
{
    uint8_t *jpeg_buf = malloc(JPEG_MAX);
    uint16_t *rgb565 = malloc(SCR_W * SCR_H * sizeof(uint16_t));
    uint8_t *chunk = malloc(4096);
    if (!jpeg_buf || !rgb565 || !chunk) {
        ESP_LOGE(TAG, "malloc failed");
        free(jpeg_buf); free(rgb565); free(chunk);
        vTaskDelete(NULL);
        return;
    }

    lv_disp_clear(0x0000);

    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd < 0) {
        ESP_LOGE(TAG, "TCP socket failed");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TCP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "TCP bind failed");
        close(server_fd);
        vTaskDelete(NULL);
        return;
    }
    listen(server_fd, 1);
    ESP_LOGI(TAG, "TCP server listening on port %d", TCP_PORT);

    while (1) {
        ESP_LOGI(TAG, "Waiting for PC connection...");
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int client_fd = accept(server_fd, (struct sockaddr *)&client, &len);
        if (client_fd < 0) continue;
        ESP_LOGI(TAG, "PC connected from %s", inet_ntoa(client.sin_addr));

        /* Start touch sender task */
        uint32_t pc_ip = client.sin_addr.s_addr;
        xTaskCreate(touch_task, "touch", 4096, (void *)(uintptr_t)pc_ip, 5, NULL);

        int state = ST_SYNC0;
        uint8_t size_buf[4];
        int size_off = 0;
        uint32_t frame_size = 0;
        uint32_t data_off = 0;
        int frames_ok = 0, frames_bad = 0;

        while (1) {
            int n = recv(client_fd, chunk, 4096, 0);
            if (n <= 0) {
                ESP_LOGI(TAG, "PC disconnected (recv=%d)", n);
                s_touch_running = false;
                break;
            }

            for (int i = 0; i < n; i++) {
                uint8_t b = chunk[i];

                switch (state) {
                case ST_SYNC0:
                    if (b == SYNC_0) state = ST_SYNC1;
                    break;
                case ST_SYNC1:
                    if (b == SYNC_1) {
                        state = ST_SIZE;
                        size_off = 0;
                    } else if (b != SYNC_0) {
                        state = ST_SYNC0;
                    }
                    break;
                case ST_SIZE:
                    size_buf[size_off++] = b;
                    if (size_off == 4) {
                        frame_size = size_buf[0] | ((uint32_t)size_buf[1] << 8) |
                                     ((uint32_t)size_buf[2] << 16) | ((uint32_t)size_buf[3] << 24);
                        if (frame_size == 0 || frame_size > JPEG_MAX) {
                            ESP_LOGW(TAG, "Bad frame size %lu", frame_size);
                            state = ST_SYNC0;
                        } else {
                            state = ST_DATA;
                            data_off = 0;
                        }
                    }
                    break;
                case ST_DATA:
                    jpeg_buf[data_off++] = b;
                    if (data_off == frame_size) {
                        JRESULT r = jpeg_decode(jpeg_buf, frame_size, rgb565, SCR_W, 0);
                        if (r == JDR_OK) {
                            lv_disp_draw_bitmap(0, 0, SCR_W, SCR_H, rgb565);
                            frames_ok++;
                        } else {
                            frames_bad++;
                        }
                        if ((frames_ok + frames_bad) % 100 == 0) {
                            ESP_LOGI(TAG, "Frames: ok=%d bad=%d", frames_ok, frames_bad);
                        }
                        state = ST_SYNC0;
                    }
                    break;
                }
            }
        }
        close(client_fd);
    }
}

/* ── Main entry ──────────────────────────────── */

esp_err_t wifi_screen_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    char ssid[33] = {0};
    char pass[65] = {0};

    if (nvs_load_credentials(ssid, pass, sizeof(ssid), sizeof(pass))) {
        ESP_LOGI(TAG, "Found saved WiFi: \"%s\"", ssid);
        if (wifi_sta_start(ssid, pass)) {
            xTaskCreate(tcp_frame_task, "tcp_frame", 4096, NULL, 5, NULL);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "STA connect failed, falling back to AP config mode");
        esp_wifi_deinit();
    }

    wifi_ap_start();
    return ESP_OK;
}
