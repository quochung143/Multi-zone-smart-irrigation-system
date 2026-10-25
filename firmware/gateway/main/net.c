// Gateway: Wi-Fi STA vào hotspot + MQTT tới Mosquitto trên laptop (docs/SERVER.md §3)
//
// - Đã nối hotspot thì ESP-NOW chạy theo kênh của hotspot (peer khai báo kênh 0 = kênh hiện tại).
// - Broker: "<ten-laptop>.local" phân giải bằng mDNS, không được thì dùng IP dự phòng.
// - LWT irr/gw/status = offline (retain); nối xong publish online. Lần nối đầu sau khi khởi động
//   publish offline rồi online để server chắc chắn resync (giống dòng "hello" của bản Serial).
// - Nối broker lỗi MQTT_RESOLVE_FAILS lần liên tiếp thì phân giải lại (IP laptop đổi theo DHCP hotspot).
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "gw.h"
#include "mdns.h"
#include "mqtt_client.h"
#include "nvs.h"

static const char *TAG = "net";

#define TOPIC_GW_STATUS  "irr/gw/status"
#define TOPIC_SRV_STATUS "irr/srv/status"
#define TOPIC_SRV_TIME   "irr/srv/time"
#define TOPIC_CMD        "irr/z/+/cmd"
#define BIT_GOT_IP       BIT0
#define BIT_RESOLVE      BIT1

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static net_config_t cfg;     // đang dùng (nạp lúc khởi động)
static net_config_t stored;  // đã lưu NVS, có thể mới hơn cfg cho tới khi khởi động lại
static net_status_t st;
static EventGroupHandle_t events;
static TimerHandle_t wifi_retry;
static esp_mqtt_client_handle_t client;
static volatile bool mqtt_up = false;
static int mqtt_fails = 0;
static bool first_connect = true;

// ---------- cấu hình NVS ----------

static void nvs_str(nvs_handle_t h, const char *key, char *out, size_t n)
{
    size_t len = n;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

static void config_load(void)
{
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = MQTT_DEFAULT_PORT;
    strlcpy(cfg.user, "gateway", sizeof(cfg.user));
    nvs_handle_t h;
    if (nvs_open("net", NVS_READONLY, &h) != ESP_OK) return;  // chưa cấu hình
    nvs_str(h, "ssid", cfg.ssid, sizeof(cfg.ssid));
    nvs_str(h, "pass", cfg.pass, sizeof(cfg.pass));
    nvs_str(h, "host", cfg.host, sizeof(cfg.host));
    nvs_str(h, "ip", cfg.ip, sizeof(cfg.ip));
    char user[sizeof(cfg.user)];
    nvs_str(h, "user", user, sizeof(user));
    if (user[0]) strlcpy(cfg.user, user, sizeof(cfg.user));
    nvs_str(h, "mpass", cfg.mpass, sizeof(cfg.mpass));
    nvs_get_u16(h, "port", &cfg.port);
    nvs_close(h);
}

static void config_init(void)
{
    config_load();
    stored = cfg;
}

// Trả về cấu hình đã lưu: console sửa từng trường liên tiếp mà không ghi đè các trường vừa đặt
net_config_t net_config_get(void)
{
    return stored;
}

bool net_config_set(const net_config_t *c)
{
    esp_ip4_addr_t tmp;
    if (c->port == 0 || (c->ip[0] && esp_netif_str_to_ip4(c->ip, &tmp) != ESP_OK)) return false;
    nvs_handle_t h;
    if (nvs_open("net", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "host", c->host);
    nvs_set_str(h, "ip", c->ip);
    nvs_set_str(h, "user", c->user);
    nvs_set_str(h, "mpass", c->mpass);
    nvs_set_u16(h, "port", c->port);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    stored = *c;
    return true;
}

void net_get_status(net_status_t *out)
{
    taskENTER_CRITICAL(&mux);
    *out = st;
    taskEXIT_CRITICAL(&mux);
    out->mqtt = mqtt_up;
}

bool net_mqtt_connected(void)
{
    return mqtt_up;
}

// ---------- MQTT ----------

bool gw_publish(uint8_t zone, const char *kind, const char *json, int qos, bool retain)
{
    if (!server_alive() || !client) return false;
    char topic[32];
    snprintf(topic, sizeof(topic), "irr/z/%u/%s", zone, kind);
    // enqueue: không chặn task gọi; mqtt task gửi đi
    return esp_mqtt_client_enqueue(client, topic, json, 0, qos, retain, true) >= 0;
}

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT da noi %s", st.broker);
        mqtt_fails = 0;
        if (first_connect) esp_mqtt_client_publish(ev->client, TOPIC_GW_STATUS, "offline", 0, 1, 1);
        first_connect = false;
        esp_mqtt_client_publish(ev->client, TOPIC_GW_STATUS, "online", 0, 1, 1);
        esp_mqtt_client_subscribe(ev->client, TOPIC_CMD, 1);
        esp_mqtt_client_subscribe(ev->client, TOPIC_SRV_TIME, 0);
        esp_mqtt_client_subscribe(ev->client, TOPIC_SRV_STATUS, 1);
        mqtt_up = true;
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (mqtt_up) ESP_LOGW(TAG, "MQTT mat ket noi");
        mqtt_up = false;
        cmd_on_server_status(false);
        if (++mqtt_fails >= MQTT_RESOLVE_FAILS) {
            mqtt_fails = 0;
            xEventGroupSetBits(events, BIT_RESOLVE);
        }
        break;
    case MQTT_EVENT_DATA:
        // Bản tin của server đều ngắn: bỏ qua bản tin bị chia mảnh
        if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) break;
        if (ev->topic_len == strlen(TOPIC_SRV_STATUS) && memcmp(ev->topic, TOPIC_SRV_STATUS, ev->topic_len) == 0) {
            bool online = ev->data_len == 6 && memcmp(ev->data, "online", 6) == 0;
            ESP_LOGI(TAG, "server %s", online ? "online" : "offline");
            cmd_on_server_status(online);
        } else {
            cmd_handle_json(ev->data, ev->data_len);  // irr/srv/time hoặc irr/z/{zone}/cmd
        }
        break;
    default:
        break;
    }
}

// Tên laptop -> IP. "<ten>.local" hỏi mDNS; IP thì dùng luôn; lỗi thì dùng IP dự phòng.
static bool resolve_broker(char *out, size_t n)
{
    esp_ip4_addr_t addr;
    if (cfg.host[0] && esp_netif_str_to_ip4(cfg.host, &addr) == ESP_OK) {
        strlcpy(out, cfg.host, n);
        return true;
    }
    size_t len = strlen(cfg.host);
    if (len > 6 && strcmp(cfg.host + len - 6, ".local") == 0) {
        char name[sizeof(cfg.host)];
        strlcpy(name, cfg.host, len - 6 + 1);
        if (mdns_query_a(name, MDNS_TIMEOUT_MS, &addr) == ESP_OK) {
            snprintf(out, n, IPSTR, IP2STR(&addr));
            ESP_LOGI(TAG, "mDNS %s -> %s", cfg.host, out);
            return true;
        }
        ESP_LOGW(TAG, "mDNS khong tim thay %s", cfg.host);
    } else if (cfg.host[0]) {
        strlcpy(out, cfg.host, n);  // tên DNS thường: để esp-mqtt tự phân giải
        return true;
    }
    if (!cfg.ip[0]) return false;
    strlcpy(out, cfg.ip, n);
    return true;
}

static void mqtt_restart(const char *addr)
{
    if (client) {
        mqtt_up = false;
        esp_mqtt_client_destroy(client);
        client = NULL;
    }
    char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", addr, cfg.port);
    taskENTER_CRITICAL(&mux);
    snprintf(st.broker, sizeof(st.broker), "%s:%u", addr, cfg.port);
    taskEXIT_CRITICAL(&mux);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .credentials.client_id = "irr-gateway",
        .credentials.username = cfg.user[0] ? cfg.user : NULL,
        .credentials.authentication.password = cfg.mpass[0] ? cfg.mpass : NULL,
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will = {.topic = TOPIC_GW_STATUS, .msg = "offline", .msg_len = 7, .qos = 1, .retain = 1},
        .network.reconnect_timeout_ms = MQTT_RETRY_MS,
    };
    client = esp_mqtt_client_init(&mc);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_mqtt_client_start(client);
    ESP_LOGI(TAG, "MQTT dang noi %s", uri);
}

// Có IP mới, hoặc nối broker lỗi nhiều lần: phân giải lại rồi khởi động lại client MQTT
static void net_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(events, BIT_GOT_IP | BIT_RESOLVE, pdTRUE, pdFALSE, portMAX_DELAY);
        if (!st.wifi) continue;
        char addr[64];
        if (!resolve_broker(addr, sizeof(addr))) {
            ESP_LOGW(TAG, "Chua co dia chi broker: go 'broker <ten>.local' hoac 'broker <ip>' tren console");
            continue;
        }
        mqtt_restart(addr);
    }
}

// ---------- Wi-Fi ----------

static void wifi_retry_cb(TimerHandle_t t)
{
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        if (st.wifi) ESP_LOGW(TAG, "Mat Wi-Fi (ly do %d)", d->reason);
        taskENTER_CRITICAL(&mux);
        st.wifi = false;
        st.ip[0] = '\0';
        taskEXIT_CRITICAL(&mux);
        xTimerStart(wifi_retry, 0);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        uint8_t primary;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&primary, &second);
        taskENTER_CRITICAL(&mux);
        st.wifi = true;
        st.channel = primary;
        snprintf(st.ip, sizeof(st.ip), IPSTR, IP2STR(&e->ip_info.ip));
        taskEXIT_CRITICAL(&mux);
        ESP_LOGI(TAG, "Wi-Fi %s: IP %s, kenh %u", cfg.ssid, st.ip, primary);
        xEventGroupSetBits(events, BIT_GOT_IP);
    }
}

void net_init(void)
{
    config_init();
    events = xEventGroupCreate();
    wifi_retry = xTimerCreate("wifi_retry", pdMS_TO_TICKS(WIFI_RETRY_MS), pdFALSE, NULL, wifi_retry_cb);
    strlcpy(st.ssid, cfg.ssid, sizeof(st.ssid));
    st.channel = ESPNOW_CHANNEL;
    if (!cfg.ssid[0]) return;  // chưa cấu hình: chỉ chạy ESP-NOW trên ESPNOW_CHANNEL

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    wifi_config_t wc = {.sta = {.threshold.authmode = WIFI_AUTH_OPEN, .pmf_cfg = {.capable = true}}};
    strlcpy((char *)wc.sta.ssid, cfg.ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg.pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

void net_start(void)
{
    if (!cfg.ssid[0]) {
        ESP_LOGW(TAG, "Chua cau hinh Wi-Fi: go 'wifi <ssid> <mat khau>' tren console");
        return;
    }
    // Tắt tiết kiệm năng lượng: modem sleep làm rớt gói ESP-NOW khi đang nối AP
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("irr-gateway");
    xTaskCreatePinnedToCore(net_task, "net", 4096, NULL, 3, NULL, 0);
}
