/*
 * 贵金属实时行情实现（黄金 / 白银）
 *
 * 三个免 API Key 的接口：
 *   1) https://api.gold-api.com/price/XAU          → 黄金 美元/盎司
 *   2) https://api.gold-api.com/price/XAG          → 白银 美元/盎司
 *   3) https://api.exchangerate-api.com/v4/latest/USD → USD/CNY 汇率
 * 两个站点的证书链都在 esp_crt_bundle 中（Let's Encrypt），可直接校验。
 * 金银价 60 秒刷新一次，汇率 1 小时刷新一次（汇率日内变化很小）。
 * 只在 WiFi 已连接且 SNTP 已对时后请求，HTTPS 走证书 bundle 校验。
 */

#include "gold_manager.h"

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

static const char *TAG = "METALS";

#define SINA_URL    "https://hq.sinajs.cn/list=hf_XAU,hf_XAG"
#define FX_URL      "https://api.exchangerate-api.com/v4/latest/USD"

#define METALS_BODY_MAX     2048                 /* 汇率响应约 2.2KB，CNY 在 600 字节处 */
#define METALS_INTERVAL_MS  (60 * 1000)          /* 金银价 60 秒刷新 */
#define METALS_RETRY_MS     (15 * 1000)          /* 失败/未联网 15 秒后重试 */
#define FX_INTERVAL_S       (60 * 60)            /* 汇率 1 小时刷新 */

#define TROY_OUNCE_GRAM     31.1034768f

static SemaphoreHandle_t s_lock;
static metals_info_t s_info;

/* 在 JSON 文本里取 "key": 后面的数字（只处理扁平字段，够用） */
static bool json_number(const char *json, const char *key, double *out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(json, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

/* 发一次 GET，把响应体读进 buf（自动截断），返回 HTTP 状态码，失败返回 -1 */
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
    esp_http_client_set_header(client, "Referer", "https://finance.sina.com.cn");

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

/* 拉取一次 USD/CNY 汇率，成功返回 true */
static bool fetch_fx(float *out)
{
    static char body[METALS_BODY_MAX];
    if (http_get(FX_URL, body, sizeof(body)) != 200) {
        return false;
    }
    double fx = 0;
    if (!json_number(body, "CNY", &fx) || fx <= 0) {
        ESP_LOGW(TAG, "汇率解析失败: %s", body);
        return false;
    }
    *out = (float)fx;
    ESP_LOGI(TAG, "USD/CNY = %.4f", fx);
    return true;
}

/* 从新浪行情文本 hq_str_hf_XAU="4116.30,..." 里取第一个数字（现价） */
static bool sina_value(const char *body, const char *symbol, double *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "hq_str_%s=\"", symbol);

    const char *p = strstr(body, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);

    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return false;
    }
    *out = v;
    return true;
}

/* 一次请求同时拿到伦敦金、伦敦银（美元/盎司） */
static bool fetch_quotes(float *gold, float *silver)
{
    static char body[METALS_BODY_MAX];
    if (http_get(SINA_URL, body, sizeof(body)) != 200) {
        return false;
    }

    double g = 0, s = 0;
    if (!sina_value(body, "hf_XAU", &g) || !sina_value(body, "hf_XAG", &s) ||
        g <= 0 || s <= 0) {
        ESP_LOGW(TAG, "报价解析失败: %.120s", body);
        return false;
    }

    *gold = (float)g;
    *silver = (float)s;
    return true;
}

/* 拉取金银价并换算成人民币/克，成功返回 true */
static bool fetch_once(float usd_cny)
{
    float gold = 0, silver = 0;
    if (!fetch_quotes(&gold, &silver)) {
        return false;
    }

    metals_info_t info = {
        .valid = true,
        .gold_usd_per_oz = gold,
        .gold_cny_per_gram = gold * usd_cny / TROY_OUNCE_GRAM,
        .silver_usd_per_oz = silver,
        .silver_cny_per_gram = silver * usd_cny / TROY_OUNCE_GRAM,
        .usd_cny = usd_cny,
    };

    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_info = info;
        xSemaphoreGive(s_lock);
        ok = true;
    }
    if (ok) {
        ESP_LOGI(TAG, "黄金 $%.2f/oz ≈ ¥%.2f/g | 白银 $%.2f/oz ≈ ¥%.2f/g (汇率 %.4f)",
                 info.gold_usd_per_oz, info.gold_cny_per_gram,
                 info.silver_usd_per_oz, info.silver_cny_per_gram, info.usd_cny);
    }
    return ok;
}

static void metals_task(void *arg)
{
    (void)arg;

    float fx = 0;
    time_t fx_time = 0;

    for (;;) {
        bool ok = false;
        if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED && wifi_mgr_time_synced()) {
            time_t now = time(NULL);
            if (fx <= 0 || now - fx_time > FX_INTERVAL_S) {
                if (fetch_fx(&fx)) {
                    fx_time = now;
                }
            }
            if (fx > 0) {
                ok = fetch_once(fx);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? METALS_INTERVAL_MS : METALS_RETRY_MS));
    }
}

void metals_mgr_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();

    /* 任务栈放 PSRAM：HTTPS 握手时内部 RAM 紧张，避免挤占 LVGL 的 DMA 缓冲 */
    if (xTaskCreateWithCaps(metals_task, "metals", 6144, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "贵金属行情任务创建失败");
    }
    ESP_LOGI(TAG, "贵金属行情服务已启动（黄金 + 白银）");
}

bool metals_mgr_get(metals_info_t *out)
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
