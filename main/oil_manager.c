/*
 * 中国石化油价实现（江苏 92# / 95#）
 *
 * 接口免 Key，返回江苏全省最新的 92#/95# 零售价。
 * 油价按调价周期（约 10 个工作日）变动，30 分钟刷新一次足够。
 * 只在 WiFi 已连接且 SNTP 已对时后请求，HTTPS 走证书 bundle 校验。
 */

#include "oil_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "wifi_manager.h"

static const char *TAG = "OIL";

#define OIL_URL          "https://api.qqsuu.cn/api/dm-oilprice?prov=%E6%B1%9F%E8%8B%8F"
#define OIL_BODY_MAX     1024
#define OIL_INTERVAL_MS  (30 * 60 * 1000)   /* 成功：30 分钟刷新 */
#define OIL_RETRY_MS     (15 * 1000)        /* 失败/未联网：15 秒后重试 */

static SemaphoreHandle_t s_lock;
static oil_info_t s_info;

/* 取 "key":"value" 形式的字符串值（只处理扁平字段，够用） */
static bool json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);

    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= cap) {
        return false;
    }
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return true;
}

static int http_get(const char *url, char *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "HTTP 客户端创建失败");
        return -1;
    }
    esp_http_client_set_header(client, "User-Agent", "esp32-dashboard/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    int status = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            size_t total = 0;
            while (total < cap - 1) {
                int r = esp_http_client_read(client, buf + total, cap - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            buf[total] = '\0';
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "HTTPS 连接失败: %s", url);
    }

    esp_http_client_cleanup(client);
    return status;
}

static bool fetch_once(void)
{
    static char body[OIL_BODY_MAX];
    if (http_get(OIL_URL, body, sizeof(body)) != 200) {
        return false;
    }

    char s92[12], s95[12];
    if (!json_string(body, "p92", s92, sizeof(s92)) ||
        !json_string(body, "p95", s95, sizeof(s95))) {
        ESP_LOGW(TAG, "解析失败: %s", body);
        return false;
    }

    float p92 = strtof(s92, NULL);
    float p95 = strtof(s95, NULL);
    if (p92 <= 0 || p95 <= 0) {
        ESP_LOGW(TAG, "无效油价: 92#=%s 95#=%s", s92, s95);
        return false;
    }

    oil_info_t info = { .valid = true, .p92 = p92, .p95 = p95 };
    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_info = info;
        xSemaphoreGive(s_lock);
        ok = true;
    }
    if (ok) {
        ESP_LOGI(TAG, "江苏油价: 92# %.2f 元/升 | 95# %.2f 元/升", p92, p95);
    }
    return ok;
}

static void oil_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool ok = false;
        if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED && wifi_mgr_time_synced()) {
            ok = fetch_once();
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? OIL_INTERVAL_MS : OIL_RETRY_MS));
    }
}

void oil_mgr_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();

    /* 任务栈放 PSRAM，避免挤占内部 RAM 与 LVGL 的 DMA 缓冲 */
    if (xTaskCreateWithCaps(oil_task, "oil", 5120, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "油价任务创建失败");
    }
    ESP_LOGI(TAG, "油价服务已启动（江苏 92#/95#）");
}

bool oil_mgr_get(oil_info_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    bool valid = false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_info;
        valid = s_info.valid;
        xSemaphoreGive(s_lock);
    }
    return valid;
}
