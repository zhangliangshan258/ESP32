/*
 * WiFi 连接管理器实现
 *
 * 事件流：
 *   esp_wifi_scan_start() --(WIFI_EVENT_SCAN_DONE)--> 缓存 AP 列表，版本号 +1
 *   esp_wifi_connect()    --(IP_EVENT_STA_GOT_IP)----> 状态置 CONNECTED，启动 SNTP
 */

#include "wifi_manager.h"
#include "wifi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_MGR";

#define WIFI_MGR_MAX_APS 24

static SemaphoreHandle_t s_lock;
static wifi_ap_record_t s_aps[WIFI_MGR_MAX_APS];    // 对外提供的扫描结果
static uint16_t s_ap_count;
static wifi_ap_record_t s_scan_buf[WIFI_MGR_MAX_APS]; // 扫描回调里的临时缓冲

static volatile uint32_t s_scan_gen;
static volatile bool s_scanning;
static volatile wifi_mgr_state_t s_state;
static volatile bool s_time_synced;
static bool s_sntp_started;
static char s_connected_ssid[33];
static bool s_inited;

/* SNTP 完成一次校时后的回调 */
static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP 对时完成");
}

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    s_sntp_started = true;

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    ESP_LOGI(TAG, "启动 SNTP: ntp.aliyun.com");
}

/* 按 RSSI 从强到弱排序（条数最多 24，插入排序足够） */
static void sort_aps(wifi_ap_record_t *aps, uint16_t n)
{
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = aps[i];
        int j = (int)i - 1;
        while (j >= 0 && aps[j].rssi < key.rssi) {
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        s_scanning = false;

        uint16_t num = 0;
        esp_wifi_scan_get_ap_num(&num);
        if (num > WIFI_MGR_MAX_APS) {
            num = WIFI_MGR_MAX_APS;
        }
        uint16_t got = num;
        if (esp_wifi_scan_get_ap_records(&got, s_scan_buf) == ESP_OK) {
            sort_aps(s_scan_buf, got);
            if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE) {
                memcpy(s_aps, s_scan_buf, got * sizeof(wifi_ap_record_t));
                s_ap_count = got;
                xSemaphoreGive(s_lock);
            }
        }
        s_scan_gen++;
        ESP_LOGI(TAG, "扫描完成，找到 %u 个 AP", (unsigned)got);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
        if (s_state == WIFI_MGR_CONNECTING || s_state == WIFI_MGR_CONNECTED) {
            s_state = WIFI_MGR_FAILED;
            s_connected_ssid[0] = '\0';
            ESP_LOGW(TAG, "WiFi 断开, reason=%d", evt ? evt->reason : -1);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_state = WIFI_MGR_CONNECTED;
        s_time_synced = false;

        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_connected_ssid, ap.ssid, sizeof(s_connected_ssid) - 1);
            s_connected_ssid[sizeof(s_connected_ssid) - 1] = '\0';
        }
        if (evt) {
            ESP_LOGI(TAG, "已联网: %s, IP " IPSTR, s_connected_ssid, IP2STR(&evt->ip_info.ip));
        }

        start_sntp();
        return;
    }
}

void wifi_mgr_init(void)
{
    if (s_inited) {
        return;
    }
    s_inited = true;

    s_lock = xSemaphoreCreateMutex();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* 网络时间用中国标准时间 */
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_LOGI(TAG, "WiFi STA 初始化完成");

#if WIFI_AUTO_CONNECT_ENABLE
    /* 开机自动连接配置文件里的网络 */
    wifi_mgr_connect(WIFI_AUTO_SSID, WIFI_AUTO_PASSWORD);
#endif
}

bool wifi_mgr_scan_start(void)
{
    if (!s_inited || s_scanning) {
        return false;
    }
    /* 正在连接时驱动不允许扫描，静默跳过（面板打开时会再扫） */
    if (s_state == WIFI_MGR_CONNECTING) {
        return false;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 60,
        .scan_time.active.max = 150,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动扫描失败: %s", esp_err_to_name(err));
        return false;
    }
    s_scanning = true;
    return true;
}

bool wifi_mgr_is_scanning(void)
{
    return s_scanning;
}

uint16_t wifi_mgr_get_aps(wifi_ap_record_t *out, uint16_t max_count)
{
    if (!out || !s_lock) {
        return 0;
    }
    uint16_t n = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = s_ap_count < max_count ? s_ap_count : max_count;
        memcpy(out, s_aps, n * sizeof(wifi_ap_record_t));
        xSemaphoreGive(s_lock);
    }
    return n;
}

uint32_t wifi_mgr_scan_generation(void)
{
    return s_scan_gen;
}

wifi_mgr_state_t wifi_mgr_get_state(void)
{
    return s_state;
}

bool wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!s_inited || !ssid || ssid[0] == '\0') {
        return false;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置 WiFi 配置失败: %s", esp_err_to_name(err));
        s_state = WIFI_MGR_FAILED;
        return false;
    }

    s_state = WIFI_MGR_CONNECTING;
    s_connected_ssid[0] = '\0';

    esp_wifi_disconnect();   /* 已连接时先断开，保证按新参数重连 */
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "发起连接失败: %s", esp_err_to_name(err));
        s_state = WIFI_MGR_FAILED;
        return false;
    }

    ESP_LOGI(TAG, "正在连接 %s", ssid);
    return true;
}

const char *wifi_mgr_connected_ssid(void)
{
    return s_connected_ssid;
}

bool wifi_mgr_time_synced(void)
{
    if (s_time_synced) {
        return true;
    }
    time_t now = 0;
    time(&now);
    return now > 1700000000;   /* 2023-11 之后视为已校时 */
}

bool wifi_mgr_get_localtime(struct tm *out)
{
    if (!out) {
        return false;
    }
    time_t now = 0;
    time(&now);
    if (now <= 1700000000) {
        return false;
    }
    localtime_r(&now, out);
    return true;
}
