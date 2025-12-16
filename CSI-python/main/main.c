#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <unistd.h>
#include <arpa/inet.h>
#include "ping/ping_sock.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#define CONFIG_PATH "/spiffs/config.json"

struct timeval tv;
static const char *TAG = "CSI_MQTT";
static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;
static bool wifi_connected = false;
static bool mqtt_status = true;
static char payload[1024];
static wifi_csi_info_t latest_csi_info;
static int number = 0;

static uint8_t wifi_mac[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
static char wifi_ssid[33] = "";
static char wifi_pass[65] = "";
static char publish_topic[128] = "ALLCLEAR/TEST";
static const char *mqtt_uri_fixed = "mqtt://allclear.sytes.net:4341";
static const char *PUBLISHER = NULL;

unsigned long startTimestamp, currentTimestamp;

typedef struct {
    unsigned frame_ctrl:16;
    unsigned duration_id:16;
    uint8_t addr1[6];
    uint8_t addr2[6];
    uint8_t addr3[6];
    unsigned seq_ctrl:16;
    uint8_t addr4[6];
} wifi_ieee80211_mac_hdr_t;

typedef struct {
    wifi_ieee80211_mac_hdr_t hdr;
    uint8_t payload[0];
} wifi_ieee80211_packet_t;

static bool parse_mac_str(const char *s, uint8_t out[6]) {
    int v[6];
    if (!s) return false;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
    for (int i=0;i<6;i++) out[i]=(uint8_t)v[i];
    return true;
}

static void spiffs_init(void) {
    esp_vfs_spiffs_conf_t conf = { .base_path="/spiffs", .partition_label=NULL, .max_files=4, .format_if_mount_failed=true };
    esp_vfs_spiffs_register(&conf);
}

static void load_config(void) {
    spiffs_init();
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if (sz<=0 || sz>4096) { fclose(f); return; }
    char *buf = malloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    cJSON *root = cJSON_Parse(buf); if (!root) { free(buf); return; }

    cJSON *n;
    n=cJSON_GetObjectItemCaseSensitive(root,"wifi_ssid"); if (cJSON_IsString(n)&&n->valuestring) { strncpy(wifi_ssid,n->valuestring,sizeof(wifi_ssid)-1); wifi_ssid[sizeof(wifi_ssid)-1]=0; }
    n=cJSON_GetObjectItemCaseSensitive(root,"wifi_password"); if (cJSON_IsString(n)&&n->valuestring) { strncpy(wifi_pass,n->valuestring,sizeof(wifi_pass)-1); wifi_pass[sizeof(wifi_pass)-1]=0; }
    n=cJSON_GetObjectItemCaseSensitive(root,"publish_topic"); if (cJSON_IsString(n)&&n->valuestring) { strncpy(publish_topic,n->valuestring,sizeof(publish_topic)-1); publish_topic[sizeof(publish_topic)-1]=0; }
    n=cJSON_GetObjectItemCaseSensitive(root,"wifi_mac"); if (cJSON_IsString(n)&&n->valuestring) { uint8_t t[6]; if (parse_mac_str(n->valuestring,t)) memcpy(wifi_mac,t,6); }

    cJSON_Delete(root); free(buf);
}

void time_sync_notification_cb(struct timeval *tv) { ESP_LOGI(TAG, "Time synchronized"); }

void initialize_sntp(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "kr.pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

void obtain_time(void) {
    initialize_sntp();
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 30;
    while (timeinfo.tm_year < (2023 - 1900) && ++retry < retry_count) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }
    setenv("TZ", "KST-9", 1);
    tzset();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DATA: {
        char payload_mqtt[128];
        snprintf(payload_mqtt, sizeof(payload_mqtt), "%.*s", event->data_len, event->data);
        char *received_pub = strtok(payload_mqtt, "/");
        char *received_status = strtok(NULL, "/");
        if (received_pub && strcmp(received_pub, PUBLISHER) == 0) {
            if (received_status && strcmp(received_status, "On") == 0) {
                ESP_LOGI(TAG, "MQTT Status: ON");
            } else if (received_status && strcmp(received_status, "Off") == 0) {
                ESP_LOGI(TAG, "MQTT Status: OFF");
            }
        }
        break;
    }
    default:
        break;
    }
}

static void wifi_csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || info->len == 0) return;
    const wifi_ieee80211_packet_t *packet = (wifi_ieee80211_packet_t *)info->buf;
    int frame_ctrl = packet->hdr.frame_ctrl;
    int frame_type = (frame_ctrl & 0x000C) >> 2;
    int frame_subtype = (frame_ctrl & 0x00F0) >> 4;
    static int packet_count = 0;
    static int64_t start_time_us = 0;
    int64_t now_us = esp_timer_get_time();
    if (start_time_us == 0) start_time_us = now_us;
    if (memcmp(info->mac, wifi_mac, sizeof(wifi_mac)) == 0) {
        packet_count++;
        int64_t elapsed_us = now_us - start_time_us;
        if (elapsed_us >= 1000000) {
            float frequency = (float)packet_count / (elapsed_us / 1000000.0f);
            ESP_LOGI(TAG, "CSI Packet Frequency: %.2f packets/sec | Frame Type: %d, Subtype: %d",
                     frequency, frame_type, frame_subtype);
            start_time_us = now_us;
            packet_count = 0;
        }
        latest_csi_info = *info;
    }
}

static void csi_task(void *pvParameters) {
    startTimestamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    while (true) {
        if (!mqtt_connected) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        memset(payload, 0, sizeof(payload));
        gettimeofday(&tv, NULL);
        time_t nowtime = tv.tv_sec;
        struct tm *nowtm = localtime(&nowtime);
        currentTimestamp = tv.tv_sec * 1000 + tv.tv_usec / 1000;
        int year = (nowtm->tm_year + 1900) % 100;
        int month = nowtm->tm_mon + 1;
        int day = nowtm->tm_mday;
        int hour = nowtm->tm_hour;
        int minute = nowtm->tm_min;
        int second = nowtm->tm_sec;
        int milliseconds = tv.tv_usec / 1000;
        char timestamp[64];
        snprintf(timestamp, sizeof(timestamp), "%02d%02d%02d%02d%02d%02d%03d",
                 year, month, day, hour, minute, second, milliseconds);
        int len = snprintf(payload, sizeof(payload),
                        "CSI data: mac=" MACSTR ", number=%d, rssi=%d, channel=%d, rate=%d, sig_mode=%d, mcs=%d, bandwidth=%d, smoothing=%d, not_sounding=%d, aggregation=%d, stbc=%d, fec_coding=%d, sgi=%d, leng=%d, time=%s\nCSI values: ",
                        MAC2STR(latest_csi_info.mac), number, latest_csi_info.rx_ctrl.rssi, latest_csi_info.rx_ctrl.channel, latest_csi_info.rx_ctrl.rate, latest_csi_info.rx_ctrl.sig_mode, latest_csi_info.rx_ctrl.mcs, latest_csi_info.rx_ctrl.cwb,
                        latest_csi_info.rx_ctrl.smoothing, latest_csi_info.rx_ctrl.not_sounding, latest_csi_info.rx_ctrl.aggregation, latest_csi_info.rx_ctrl.stbc, latest_csi_info.rx_ctrl.fec_coding, latest_csi_info.rx_ctrl.sgi, latest_csi_info.len, timestamp);
        for (int i = 4; i < latest_csi_info.len; ++i) {
            len += snprintf(payload + len, sizeof(payload) - len, "%d ", latest_csi_info.buf[i]);
        }
        if (mqtt_status) esp_mqtt_client_publish(mqtt_client, publish_topic, payload, len, 1, 0);
        vTaskDelay(pdMS_TO_TICKS(25));
        printf("%s\n", payload);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        ESP_LOGI(TAG, "Disconnected from the AP, retrying...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Connected to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        wifi_connected = true;
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGI(TAG, "Connected to AP: %s", ap_info.ssid);
            ESP_LOGI(TAG, "AP MAC Address: " MACSTR, MAC2STR(ap_info.bssid));
        }
    }
}

static void wifi_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_netif_create_default_wifi_sta();
    wifi_config_t sta_config = {0};
    snprintf((char*)sta_config.sta.ssid, sizeof(sta_config.sta.ssid), "%s", wifi_ssid);
    snprintf((char*)sta_config.sta.password, sizeof(sta_config.sta.password), "%s", wifi_pass);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_csi_config_t csi_config = {
        .lltf_en = 1,
        .htltf_en = 0,
        .stbc_htltf2_en = 0,
        .ltf_merge_en = 0,
        .channel_filter_en = 0,
        .manu_scale = 0,
        .shift = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_cb, NULL));
}

static void mqtt_app_start(void) {
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = mqtt_uri_fixed,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    while (true) {
        esp_err_t err = esp_mqtt_client_start(mqtt_client);
        if (err == ESP_OK) { ESP_LOGI(TAG, "Connected to MQTT broker"); mqtt_connected = true; break; }
        ESP_LOGE(TAG, "Failed to connect to MQTT broker, retrying...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void) {
    load_config();
    wifi_init();
    PUBLISHER = publish_topic;
    while (!wifi_connected) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    mqtt_app_start();
    xTaskCreate(csi_task, "csi_task", 4096, NULL, 5, NULL);
}