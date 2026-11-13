/*
 * 在线天气实现（Open-Meteo，免 API Key）
 *
 * 坐标取常州市武进区中心附近：31.7175N, 119.9421E
 * 只在 WiFi 已连接且 SNTP 已对时后请求，HTTPS 走证书 bundle 校验。
 */

#include "weather_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "wifi_manager.h"

static const char *TAG = "WEATHER";

#define WEATHER_BODY_MAX   1024
#define WEATHER_INTERVAL_MS (15 * 60 * 1000)   /* 成功后 15 分钟刷新 */
#define WEATHER_RETRY_MS    (15 * 1000)        /* 失败/未联网 15 秒后重试 */

static SemaphoreHandle_t s_lock;
static weather_info_t s_info;
static TaskHandle_t s_task;

/* 当前定位：默认常州市武进区，可由网页修改 */
static char s_label[48] = "常州 武进";
static double s_lat = 31.7175;
static double s_lon = 119.9421;

/* WMO 天气代码 → 短英文描述
 * 注意：中文字体只包含 ~1187 个常用字，天气用字（云/阴/雷/雾…）大多缺失，
 * 这里统一用 ASCII 短词，保证一定能渲染出来。 */
static const char *wmo_desc(int code)
{
    switch (code) {
    case 0:             return "CLEAR";
    case 1:             return "MOSTLY CLR";
    case 2:             return "PARTLY CLD";
    case 3:             return "OVERCAST";
    case 45:
    case 48:            return "FOG";
    case 51:
    case 53:
    case 55:            return "DRIZZLE";
    case 56:
    case 57:            return "FZ DRIZ";
    case 61:            return "LT RAIN";
    case 63:            return "RAIN";
    case 65:            return "HVY RAIN";
    case 66:
    case 67:            return "FZ RAIN";
    case 71:
    case 73:            return "SNOW";
    case 75:            return "HVY SNOW";
    case 77:            return "SNW GRAIN";
    case 80:
    case 81:            return "SHOWERS";
    case 82:            return "HVY SHWR";
    case 85:
    case 86:            return "SNW SHWR";
    case 95:
    case 96:
    case 99:            return "T-STORM";
    default:            return "N/A";
    }
}

/* 在 JSON 文本里取 "key": 后面的数字（只处理扁平字段，够用） */
static bool json_number(const char *scope, const char *key, double *out)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(scope, pattern);
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

/* 解析 "key":[...] 形式的数字数组，返回个数 */
static int json_array(const char *scope, const char *key, double *out, int max)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":[", key);

    const char *p = strstr(scope, pattern);
    if (!p) {
        return 0;
    }
    p += strlen(pattern);

    int n = 0;
    while (n < max && *p) {
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) {
            break;
        }
        out[n++] = v;
        p = end;
        while (*p == ',' || *p == ' ') {
            p++;
        }
        if (*p == ']') {
            break;
        }
    }
    return n;
}

static bool parse_response(const char *json)
{
    /* 响应里 current_units 也含同名字段，必须先定位到 "current" 对象再取数 */
    const char *cur = strstr(json, "\"current\":");
    if (!cur) {
        ESP_LOGW(TAG, "响应里没有 current 字段");
        return false;
    }

    double t = 0, h = 0, c = 0;
    if (!json_number(cur, "temperature_2m", &t) ||
        !json_number(cur, "relative_humidity_2m", &h) ||
        !json_number(cur, "weather_code", &c)) {
        ESP_LOGW(TAG, "天气字段解析失败");
        return false;
    }

    weather_info_t info = {
        .valid = true,
        .temp_c = (int)(t >= 0 ? t + 0.5 : t - 0.5),
        .humidity = (int)(h + 0.5),
        .code = (int)(c + 0.5),
    };
    strncpy(info.desc, wmo_desc(info.code), sizeof(info.desc) - 1);
    info.desc[sizeof(info.desc) - 1] = '\0';

    /* 未来 3 天预报：daily 数组第 0 项是今天，取 1..3 */
    const char *daily = strstr(json, "\"daily\":");
    if (daily) {
        double codes[4], tmax[4], tmin[4];
        int nc = json_array(daily, "weather_code", codes, 4);
        int nx = json_array(daily, "temperature_2m_max", tmax, 4);
        int nn = json_array(daily, "temperature_2m_min", tmin, 4);
        int n = nc < nx ? nc : nx;
        if (nn < n) {
            n = nn;
        }
        for (int i = 0; i < 3 && i + 1 < n; i++) {
            info.daily[i].code = (int)(codes[i + 1] + 0.5);
            info.daily[i].tmax = (int)(tmax[i + 1] + 0.5);
            info.daily[i].tmin = (int)(tmin[i + 1] + 0.5);
        }
    }

    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_info = info;
        xSemaphoreGive(s_lock);
        ok = true;
    }
    if (ok) {
        ESP_LOGI(TAG, "%s 天气: %d°C %s 湿度 %d%% (code %d)",
                 s_label, info.temp_c, info.desc, info.humidity, info.code);
    }
    return ok;
}

static bool fetch_once(void)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&forecast_days=4"
             "&timezone=Asia%%2FShanghai",
             s_lat, s_lon);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 512,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGW(TAG, "HTTP 客户端创建失败");
        return false;
    }
    esp_http_client_set_header(client, "User-Agent", "esp32-dashboard/1.0");
    esp_http_client_set_header(client, "Accept", "application/json");

    bool ok = false;
    static char body[WEATHER_BODY_MAX];

    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int total = 0;
            while (total < (int)sizeof(body) - 1) {
                int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            body[total] = '\0';
            ok = parse_response(body);
        } else {
            ESP_LOGW(TAG, "HTTP 状态码 %d", status);
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "HTTPS 连接失败");
    }

    esp_http_client_cleanup(client);
    return ok;
}

static void weather_task(void *arg)
{
    (void)arg;

    for (;;) {
        bool ok = false;
        if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED && wifi_mgr_time_synced()) {
            ok = fetch_once();
        }
        /* 用任务通知代替延时：网页改了城市可以立刻醒来重拉 */
        uint32_t wait_ms = ok ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

void weather_mgr_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();

    /* 任务栈放 PSRAM：HTTPS 握手时内部 RAM 紧张，避免抢占 LVGL 绘制用的
     * SPI DMA 缓冲（否则会出现 Draw bitmap failed） */
    if (xTaskCreateWithCaps(weather_task, "weather", 6144, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "天气任务创建失败");
    }
    ESP_LOGI(TAG, "天气服务已启动（%s）", s_label);
}

void weather_mgr_set_location(double lat, double lon, const char *label)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    s_lat = lat;
    s_lon = lon;
    if (label && label[0]) {
        strncpy(s_label, label, sizeof(s_label) - 1);
        s_label[sizeof(s_label) - 1] = '\0';
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "定位已更新: %s (%.4f, %.4f)", s_label, s_lat, s_lon);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

bool weather_mgr_get_location(double *lat, double *lon, char *label, size_t label_size)
{
    bool ok = false;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (lat) *lat = s_lat;
        if (lon) *lon = s_lon;
        if (label && label_size) {
            strncpy(label, s_label, label_size - 1);
            label[label_size - 1] = '\0';
        }
        xSemaphoreGive(s_lock);
        ok = true;
    }
    return ok;
}

bool weather_mgr_get(weather_info_t *out)
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
