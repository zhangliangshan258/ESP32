/*
 * 网页配置模块实现
 *
 * 架构：
 *   浏览器 --HTTP--> httpd 任务 --保存到 SPIFFS--> 置位/加锁 --> LVGL 任务重绘
 *
 * 头像走“原始像素”通道：网页用 Canvas 把任意图片居中裁剪成 108x108，
 * 转成 RGB565 小端后直接 POST 上来，固件不需要任何图片解码器，也不会
 * 受上传格式（JPEG/PNG/WebP）影响。
 */

#include "web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"

#include "lvgl.h"
#include "misc/cache/instance/lv_image_cache.h"   /* 替换像素后需要丢弃该图片的缓存 */
#include "esp_lv_adapter.h"

#include "dashboard_api.h"
#include "wifi_manager.h"
#include "weather_manager.h"

/* 由 main/logo_img.c 提供的默认头像 */
extern const lv_image_dsc_t logo_img;

/* 由 main/web/index.html 通过 EMBED_FILES 嵌入 */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static const char *TAG = "WEB_CFG";

#define LOGO_W            108
#define LOGO_H            108
#define LOGO_BYTES        (LOGO_W * LOGO_H * 2)

#define SPIFFS_BASE       "/spiffs"
#define SPIFFS_PART       "storage"
#define LOGO_PATH         SPIFFS_BASE "/logo.bin"
#define MOTTO_PATH        SPIFFS_BASE "/motto.txt"
#define WEATHER_PATH      SPIFFS_BASE "/weather.txt"

#define MOTTO_MAX_CHARS   12                      /* 一行放得下的汉字数 */
#define MOTTO_BUF_SIZE    128

#define WX_LABEL_CHARS    10
#define WX_DEFAULT_CITY   "常州"
#define WX_DEFAULT_DIST   "武进"
#define WX_DEFAULT_LABEL  "常州 武进"
#define WX_DEFAULT_LAT    31.7175
#define WX_DEFAULT_LON    119.9421

static bool s_mounted;
static uint8_t *s_logo_buf[2];                    /* 双缓冲，避免重绘期间释放 */
static int s_logo_idx;
static lv_image_dsc_t s_logo_dsc;
static bool s_have_custom_logo;
static char s_motto[MOTTO_BUF_SIZE] = DASH_DEFAULT_MOTTO;

/* 天气城市（网页设置，保存到 SPIFFS） */
static char s_wx_city[24] = WX_DEFAULT_CITY;
static char s_wx_district[24] = WX_DEFAULT_DIST;
static char s_wx_label[48] = WX_DEFAULT_LABEL;
static double s_wx_lat = WX_DEFAULT_LAT;
static double s_wx_lon = WX_DEFAULT_LON;

/* ------------------------------------------------------------------ */
/* 存储                                                                */
/* ------------------------------------------------------------------ */

static uint8_t *logo_alloc(void)
{
    uint8_t *p = heap_caps_malloc(LOGO_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(LOGO_BYTES, MALLOC_CAP_8BIT);
    }
    return p;
}

static bool save_file(const char *path, const void *data, size_t len)
{
    if (!s_mounted) {
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "打开 %s 失败", path);
        return false;
    }
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static void init_logo_dsc(void)
{
    memset(&s_logo_dsc, 0, sizeof(s_logo_dsc));
    s_logo_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_logo_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_logo_dsc.header.w = LOGO_W;
    s_logo_dsc.header.h = LOGO_H;
    s_logo_dsc.header.stride = LOGO_W * 2;
    s_logo_dsc.data_size = LOGO_BYTES;
    s_logo_dsc.data = NULL;
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    size_t n = strlen(src);
    if (n >= dst_size) {
        n = dst_size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* 读取保存的天气城市（city/district/label/lat/lon 的 key=value 行） */
static void load_weather_file(void)
{
    FILE *f = fopen(WEATHER_PATH, "rb");
    if (!f) {
        return;
    }

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *val = eq + 1;
        size_t n = strlen(val);
        while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' ||
                         val[n - 1] == ' ' || val[n - 1] == '\t')) {
            val[--n] = '\0';
        }

        if (!strcmp(line, "city")) {
            copy_str(s_wx_city, sizeof(s_wx_city), val);
        } else if (!strcmp(line, "district")) {
            copy_str(s_wx_district, sizeof(s_wx_district), val);
        } else if (!strcmp(line, "label")) {
            copy_str(s_wx_label, sizeof(s_wx_label), val);
        } else if (!strcmp(line, "lat")) {
            char *end = NULL;
            double d = strtod(val, &end);
            if (end != val) {
                s_wx_lat = d;
            }
        } else if (!strcmp(line, "lon")) {
            char *end = NULL;
            double d = strtod(val, &end);
            if (end != val) {
                s_wx_lon = d;
            }
        }
    }
    fclose(f);

    if (!s_wx_label[0]) {
        snprintf(s_wx_label, sizeof(s_wx_label), "%s %s", s_wx_city, s_wx_district);
    }
}

void web_config_storage_init(void)
{
    init_logo_dsc();

    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = SPIFFS_PART,
        .max_files = 3,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS 挂载失败(%s)，网页改动将无法保存", esp_err_to_name(err));
    } else {
        s_mounted = true;
    }

    if (s_mounted) {
        FILE *f = fopen(MOTTO_PATH, "rb");
        if (f) {
            size_t n = fread(s_motto, 1, sizeof(s_motto) - 1, f);
            fclose(f);
            while (n > 0 && (s_motto[n - 1] == '\n' || s_motto[n - 1] == '\r')) {
                n--;
            }
            s_motto[n] = '\0';
            if (n == 0) {
                strcpy(s_motto, DASH_DEFAULT_MOTTO);
            }
        }

        f = fopen(LOGO_PATH, "rb");
        if (f) {
            uint8_t *buf = logo_alloc();
            if (buf) {
                size_t n = fread(buf, 1, LOGO_BYTES, f);
                if (n == LOGO_BYTES) {
                    s_logo_buf[0] = buf;
                    s_have_custom_logo = true;
                    s_logo_dsc.data = buf;
                } else {
                    heap_caps_free(buf);
                }
            }
            fclose(f);
        }

        load_weather_file();
    }

    /* 天气任务还没启动，这里只是把定位写进天气模块（无需加锁） */
    weather_mgr_set_location(s_wx_lat, s_wx_lon, s_wx_label);

    ESP_LOGI(TAG, "存储%s，箴言=\"%s\"，自定义头像=%d，天气城市=%s(%.4f,%.4f)",
             s_mounted ? "已就绪" : "不可用", s_motto, (int)s_have_custom_logo,
             s_wx_label, s_wx_lat, s_wx_lon);
}

const char *web_config_get_motto(void)
{
    return s_motto;
}

const void *web_config_get_logo_src(void)
{
    return s_have_custom_logo ? (const void *)&s_logo_dsc : (const void *)&logo_img;
}

/* ------------------------------------------------------------------ */
/* LVGL 更新（持锁调用）                                                */
/* ------------------------------------------------------------------ */

static void apply_motto(const char *text)
{
    if (esp_lv_adapter_lock((uint32_t)-1) == ESP_OK) {
        dash_set_motto_text(text);
        esp_lv_adapter_unlock();
    }
}

static void apply_logo(const void *src)
{
    if (esp_lv_adapter_lock((uint32_t)-1) == ESP_OK) {
        lv_image_cache_drop(&s_logo_dsc);
        dash_set_logo_src(src);
        esp_lv_adapter_unlock();
    }
}

static void apply_weather_label(const char *text)
{
    if (esp_lv_adapter_lock((uint32_t)-1) == ESP_OK) {
        dash_set_weather_city(text);
        esp_lv_adapter_unlock();
    }
}

/* ------------------------------------------------------------------ */
/* 箴言校验                                                            */
/* ------------------------------------------------------------------ */

/* 解析一个 UTF-8 字符，返回字节数；非法字节按 1 字节跳过 */
static int utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;

    if (p[0] < 0x80) {
        *cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
              ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static bool glyph_exists(uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(&dash_motto_font, &dsc, cp, 0);
}

/* 收集字库缺失的字符：全部支持返回 true，否则把缺失字符拼进 bad */
static bool motto_collect_bad(const char *text, char *bad, size_t bad_size, int *chars_out)
{
    int chars = 0;
    size_t bad_len = 0;

    bad[0] = '\0';
    for (const char *p = text; *p; ) {
        uint32_t cp = 0;
        int len = utf8_next(p, &cp);
        chars++;

        if (cp != ' ' && cp != '\t' && !glyph_exists(cp) && bad_len + len < bad_size) {
            memcpy(bad + bad_len, p, len);
            bad_len += len;
            bad[bad_len] = '\0';
            if (bad_len >= bad_size - 8) {
                strncat(bad, "…", bad_size - strlen(bad) - 1);
                break;
            }
        }
        p += len;
    }
    *chars_out = chars;
    return bad_len == 0;
}

/* ------------------------------------------------------------------ */
/* HTTP 处理                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t send_text(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start,
                           (size_t)(index_html_end - index_html_start));
}

static esp_err_t state_get(httpd_req_t *req)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
             "motto=%s\ncustom_logo=%d\nssid=%s\nmax_chars=%d\ncity=%s\ndistrict=%s\n",
             s_motto, (int)s_have_custom_logo, wifi_mgr_connected_ssid(), MOTTO_MAX_CHARS,
             s_wx_city, s_wx_district);
    return send_text(req, "200 OK", buf);
}

static esp_err_t logo_raw_get(httpd_req_t *req)
{
    const lv_image_dsc_t *dsc = s_have_custom_logo ? &s_logo_dsc : &logo_img;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)dsc->data, LOGO_BYTES);
}

static esp_err_t motto_post(httpd_req_t *req)
{
    char body[MOTTO_BUF_SIZE];
    size_t total = req->content_len;

    if (total == 0 || total >= sizeof(body)) {
        return send_text(req, "400 Bad Request", "箴言长度不合法");
    }

    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return send_text(req, "400 Bad Request", "读取请求失败");
        }
        got += r;
    }
    body[got] = '\0';

    /* 去掉首尾空白 */
    char *text = body;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' ||
                       text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = '\0';
    }
    if (len == 0) {
        return send_text(req, "400 Bad Request", "箴言不能为空");
    }

    char bad[64];
    int chars = 0;
    if (!motto_collect_bad(text, bad, sizeof(bad), &chars)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "设备字库缺少这些字的字形：%s", bad);
        return send_text(req, "400 Bad Request", msg);
    }
    if (chars > MOTTO_MAX_CHARS) {
        char msg[96];
        snprintf(msg, sizeof(msg), "最多 %d 个字，请精简一下", MOTTO_MAX_CHARS);
        return send_text(req, "400 Bad Request", msg);
    }

    if (!save_file(MOTTO_PATH, text, strlen(text))) {
        return send_text(req, "500 Internal Server Error", "保存失败：存储不可用");
    }

    strncpy(s_motto, text, sizeof(s_motto) - 1);
    s_motto[sizeof(s_motto) - 1] = '\0';
    apply_motto(s_motto);

    ESP_LOGI(TAG, "箴言已更新: %s", s_motto);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t motto_delete(httpd_req_t *req)
{
    remove(MOTTO_PATH);
    strcpy(s_motto, DASH_DEFAULT_MOTTO);
    apply_motto(s_motto);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t logo_post(httpd_req_t *req)
{
    if (req->content_len != LOGO_BYTES) {
        char msg[96];
        snprintf(msg, sizeof(msg), "图片数据长度应为 %d 字节", LOGO_BYTES);
        return send_text(req, "400 Bad Request", msg);
    }

    int next = s_logo_idx ^ 1;
    if (!s_logo_buf[next]) {
        s_logo_buf[next] = logo_alloc();
    }
    if (!s_logo_buf[next]) {
        return send_text(req, "500 Internal Server Error", "内存不足，无法接收图片");
    }

    size_t got = 0;
    while (got < LOGO_BYTES) {
        int r = httpd_req_recv(req, (char *)s_logo_buf[next] + got, LOGO_BYTES - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return send_text(req, "400 Bad Request", "图片接收中断");
        }
        got += r;
    }

    if (!save_file(LOGO_PATH, s_logo_buf[next], LOGO_BYTES)) {
        return send_text(req, "500 Internal Server Error", "保存失败：存储不可用");
    }

    s_logo_idx = next;
    s_have_custom_logo = true;
    s_logo_dsc.data = s_logo_buf[next];
    apply_logo(&s_logo_dsc);

    ESP_LOGI(TAG, "头像已更新 (%d 字节)", (int)got);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t logo_delete(httpd_req_t *req)
{
    remove(LOGO_PATH);
    s_have_custom_logo = false;
    s_logo_dsc.data = NULL;
    apply_logo(&logo_img);
    return send_text(req, "200 OK", "OK");
}

/* ------------------------------------------------------------------ */
/* 天气城市                                                            */
/* ------------------------------------------------------------------ */

/* 正文格式（每行 key=value，由网页用浏览器解析坐标后提交）：
 *   city=常州
 *   district=武进
 *   label=常州 武进
 *   lat=31.7020406
 *   lon=119.9377542
 */
static esp_err_t weather_post(httpd_req_t *req)
{
    char body[512];
    char city[24] = "";
    char district[24] = "";
    char label[48] = "";
    double lat = 0, lon = 0;
    bool has_lat = false, has_lon = false;

    size_t total = req->content_len;
    if (total == 0 || total >= sizeof(body)) {
        return send_text(req, "400 Bad Request", "参数长度不合法");
    }

    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return send_text(req, "400 Bad Request", "读取请求失败");
        }
        got += r;
    }
    body[got] = '\0';

    char *save = NULL;
    for (char *line = strtok_r(body, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *val = eq + 1;
        if (!strcmp(line, "city")) {
            copy_str(city, sizeof(city), val);
        } else if (!strcmp(line, "district")) {
            copy_str(district, sizeof(district), val);
        } else if (!strcmp(line, "label")) {
            copy_str(label, sizeof(label), val);
        } else if (!strcmp(line, "lat")) {
            char *end = NULL;
            lat = strtod(val, &end);
            has_lat = (end != val);
        } else if (!strcmp(line, "lon")) {
            char *end = NULL;
            lon = strtod(val, &end);
            has_lon = (end != val);
        }
    }

    if (!has_lat || !has_lon || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) {
        return send_text(req, "400 Bad Request", "经纬度缺失或超出范围");
    }
    if (!city[0] && !district[0]) {
        return send_text(req, "400 Bad Request", "城市和区域不能同时为空");
    }
    if (!label[0]) {
        snprintf(label, sizeof(label), "%s %s", city, district);
    }

    char bad[64];
    int chars = 0;
    if (!motto_collect_bad(label, bad, sizeof(bad), &chars)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "设备字库缺少这些字的字形：%s", bad);
        return send_text(req, "400 Bad Request", msg);
    }
    if (chars > WX_LABEL_CHARS) {
        char msg[96];
        snprintf(msg, sizeof(msg), "城市+区域最多 %d 个字", WX_LABEL_CHARS);
        return send_text(req, "400 Bad Request", msg);
    }

    char out[192];
    int n = snprintf(out, sizeof(out),
                     "city=%s\ndistrict=%s\nlabel=%s\nlat=%.6f\nlon=%.6f\n",
                     city, district, label, lat, lon);
    if (!save_file(WEATHER_PATH, out, (size_t)n)) {
        return send_text(req, "500 Internal Server Error", "保存失败：存储不可用");
    }

    copy_str(s_wx_city, sizeof(s_wx_city), city);
    copy_str(s_wx_district, sizeof(s_wx_district), district);
    copy_str(s_wx_label, sizeof(s_wx_label), label);
    s_wx_lat = lat;
    s_wx_lon = lon;

    apply_weather_label(s_wx_label);
    weather_mgr_set_location(lat, lon, label);   // 立即刷新天气

    ESP_LOGI(TAG, "天气城市已更新: %s (%.4f, %.4f)", label, lat, lon);
    return send_text(req, "200 OK", "OK");
}

static esp_err_t weather_delete(httpd_req_t *req)
{
    remove(WEATHER_PATH);
    copy_str(s_wx_city, sizeof(s_wx_city), WX_DEFAULT_CITY);
    copy_str(s_wx_district, sizeof(s_wx_district), WX_DEFAULT_DIST);
    copy_str(s_wx_label, sizeof(s_wx_label), WX_DEFAULT_LABEL);
    s_wx_lat = WX_DEFAULT_LAT;
    s_wx_lon = WX_DEFAULT_LON;

    apply_weather_label(s_wx_label);
    weather_mgr_set_location(s_wx_lat, s_wx_lon, s_wx_label);
    return send_text(req, "200 OK", "OK");
}

void web_config_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        return;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,    .handler = index_get },
        { .uri = "/api/state",     .method = HTTP_GET,    .handler = state_get },
        { .uri = "/api/logo.raw",  .method = HTTP_GET,    .handler = logo_raw_get },
        { .uri = "/api/motto",     .method = HTTP_POST,   .handler = motto_post },
        { .uri = "/api/motto",     .method = HTTP_DELETE, .handler = motto_delete },
        { .uri = "/api/logo",      .method = HTTP_POST,   .handler = logo_post },
        { .uri = "/api/logo",      .method = HTTP_DELETE, .handler = logo_delete },
        { .uri = "/api/weather",   .method = HTTP_POST,   .handler = weather_post },
        { .uri = "/api/weather",   .method = HTTP_DELETE, .handler = weather_delete },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册 %s 失败: %s", uris[i].uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "设置页已启动，浏览器打开 http://<设备IP>/");
}
