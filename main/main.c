/*
 * SPDX-FileCopyrightText: Copyright 2026 OSPTEK
 * SPDX-License-Identifier: CC-BY-4.0
 *
 * ESP32-S3 + CO5300(QSPI) + CST820 + LVGL 9 自定义仪表盘
 *
 * 这个例子的 UI 全部用 LVGL 9 的对象（lv_obj）搭建，不依赖 lv_demos，
 * 方便你直接改造成自己的界面。代码里按“对象树 → 样式 → 布局 → 事件 → 定时器”
 * 的顺序写了注释，建议对照阅读。
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


#include "esp_log.h"
#include "esp_err.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst820.h"

#include "driver/spi_master.h"
#include "driver/i2c_master.h"

#include "esp_lv_adapter.h"
#include "lvgl.h"

#include "co5300_init_cmds.h"
#include "wifi_manager.h"
#include "ui_wifi.h"
#include "weather_manager.h"
#include "gold_manager.h"
#include "oil_manager.h"
#include "web_config.h"
#include "dashboard_api.h"

/* 品牌 logo 图像数据（由 main/logo_img.c 提供，108x108 RGB565） */
extern const lv_image_dsc_t logo_img;

/* 网页可修改的 UI：头像与箴言（字体 = 3755 常用字 20px 字库 + 内置思源黑体兜底） */
lv_font_t dash_motto_font;
static lv_obj_t *s_logo_img;
static lv_obj_t *s_motto_label;
static lv_obj_t *s_wx_city_label;

void dash_set_motto_text(const char *utf8)
{
    if (s_motto_label) {
        lv_label_set_text(s_motto_label, utf8 ? utf8 : "");
    }
}

void dash_set_weather_city(const char *utf8)
{
    if (s_wx_city_label) {
        lv_label_set_text(s_wx_city_label, utf8 ? utf8 : "");
    }
}

void dash_set_logo_src(const void *src)
{
    if (s_logo_img && src) {
        lv_image_set_src(s_logo_img, src);
    }
}

static const char *TAG = "LVGL_DASHBOARD";

// ==================== 引脚定义（本示例对应 OSPTEK 官方 ESP32-S3 开发板） ====================
#define EXAMPLE_PIN_NUM_LCD_CS      GPIO_NUM_9
#define EXAMPLE_PIN_NUM_LCD_SCK     GPIO_NUM_12
#define EXAMPLE_PIN_NUM_LCD_D0      GPIO_NUM_13   // QSPI D0
#define EXAMPLE_PIN_NUM_LCD_D1      GPIO_NUM_14   // QSPI D1
#define EXAMPLE_PIN_NUM_LCD_D2      GPIO_NUM_15   // QSPI D2
#define EXAMPLE_PIN_NUM_LCD_D3      GPIO_NUM_16   // QSPI D3
#define EXAMPLE_PIN_NUM_LCD_RST     GPIO_NUM_10
#define EXAMPLE_PIN_NUM_LCD_TE      GPIO_NUM_11

#define EXAMPLE_TOUCH_I2C_SCL       GPIO_NUM_40
#define EXAMPLE_TOUCH_I2C_SDA       GPIO_NUM_39
#define EXAMPLE_TOUCH_RST           GPIO_NUM_41
#define EXAMPLE_TOUCH_INT           GPIO_NUM_42

#define EXAMPLE_TOUCH_I2C_NUM       I2C_NUM_0
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (400 * 1000)

// ==================== 显示参数（460x460，X 方向有 10px 无效区） ====================
#define EXAMPLE_LCD_HOST            SPI2_HOST
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)

#define EXAMPLE_LCD_H_RES           460
#define EXAMPLE_LCD_V_RES           460
#define EXAMPLE_LCD_X_GAP           10
#define EXAMPLE_LCD_Y_GAP           0

// ==================== Material Design 3 Dark 调色板 ====================
// Surface Levels（紫 tint 深底体系，无边框，纯靠 elevation 区分）
#define MD3_SCR_BG      0x14101F   // Surface Level 0（深紫罗兰底）
#define MD3_CARD        0x241C33   // Surface Container（更饱满的紫）
#define MD3_CARD_HIGH   0x322547   // Surface Container High
// 文字
#define MD3_ON_SURF     0xF3EDFF   // On Surface
#define MD3_ON_SURF_V   0xCBC2E0   // On Surface Variant
// 强调色（更艳丽但同色系）
#define MD3_PRIMARY     0xC9A6FF   // 紫罗兰
#define MD3_SECONDARY   0xA9C4FF   // 银蓝（白银）
#define MD3_TERTIARY    0xFFAECE   // 蜜桃粉
// 中性色辅助
#define MD3_OUTLINE     0x8E86A8   // Outline

// 自建汉字子集：28px 用于贵金属名称与单位，20px 用于油价卡标题与单位
LV_FONT_DECLARE(font_cjk_28);
LV_FONT_DECLARE(font_cjk_20);
LV_FONT_DECLARE(font_cjk_20_full);   /* 20px 4bpp，覆盖 GB2312 一级常用字 3755 个，供网页自定义箴言 */

// ==================== UI 对象的全局句柄（定时器里要更新它们） ====================
static esp_lcd_panel_handle_t s_panel;

static lv_obj_t *s_time_label;
static lv_obj_t *s_wifi_chip_label;
static lv_obj_t *s_wx_temp;            // 天气卡：大温度
static lv_obj_t *s_wx_desc;            // 天气卡：天气描述 + 湿度
static lv_obj_t *s_wx_big_icon;        // 天气卡：右上大图标
static lv_obj_t *s_wx_fc_day[3];       // 天气卡：3 天预报的星期
static lv_obj_t *s_wx_fc_icon[3];      // 天气卡：3 天预报的图标
static lv_obj_t *s_wx_fc_text[3];      // 天气卡：3 天预报的温度范围
static lv_obj_t *s_gold_price;         // 贵金属卡：黄金 人民币/克
static lv_obj_t *s_gold_sub;           // 贵金属卡：黄金 美元/盎司
static lv_obj_t *s_silver_price;       // 贵金属卡：白银 人民币/克
static lv_obj_t *s_silver_sub;         // 贵金属卡：白银 美元/盎司
static lv_obj_t *s_oil_92;             // 油价卡：92# 价格
static lv_obj_t *s_oil_95;             // 油价卡：95# 价格

/* -------------------------------------------------------------------------
 * 小工具：创建一张“卡片”
 * LVGL 9 的控件默认带有一堆主题样式，先 lv_obj_remove_style_all() 清空，
 * 再按需加圆角/背景/边框，这是做自定义 UI 最常用的起手式。
 * ------------------------------------------------------------------------- */
static lv_obj_t *make_card(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, width, height);
    // MD3: Surface Container，无边框，elevation 靠色阶
    lv_obj_set_style_bg_color(card, lv_color_hex(MD3_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);          // MD3 shape scale Large
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* -------------------------------------------------------------------------
 * 天气图标：用基础圆点/方块拼出太阳、云、雨、雪、雷、雾
 * 全部画在 32x24 的容器内，重绘前先 lv_obj_clean()。
 * ------------------------------------------------------------------------- */
#define WX_CLOUD_COLOR  0xCAC4D0
#define WX_SUN_COLOR    0xFFD86B
#define WX_RAIN_COLOR   0x9CC7FF

static void wx_dot(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_style_all(d);
    lv_obj_set_pos(d, x, y);
    lv_obj_set_size(d, w, h);
    lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, radius, 0);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
}

/* 顶部云（给雨/雪/雷留出下方空间） */
static void wx_cloud_top(lv_obj_t *p)
{
    wx_dot(p, 4, 3, 9, 9, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 9, 0, 12, 12, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 18, 4, 8, 8, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 6, 8, 20, 5, WX_CLOUD_COLOR, 2);
}

/* 居中云（阴天 / 多云） */
static void wx_cloud_mid(lv_obj_t *p)
{
    wx_dot(p, 4, 8, 10, 10, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 10, 4, 13, 13, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 19, 9, 9, 9, WX_CLOUD_COLOR, LV_RADIUS_CIRCLE);
    wx_dot(p, 6, 13, 21, 6, WX_CLOUD_COLOR, 3);
}

static void wx_set_icon(lv_obj_t *box, int code)
{
    lv_obj_clean(box);

    if (code == 0 || code == 1) {
        /* 晴 */
        wx_dot(box, 10, 6, 12, 12, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 15, 0, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 15, 21, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 4, 10, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 26, 10, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 7, 2, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 23, 2, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 7, 18, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        wx_dot(box, 23, 18, 3, 3, WX_SUN_COLOR, LV_RADIUS_CIRCLE);
        return;
    }

    if (code == 45 || code == 48) {
        /* 雾 */
        wx_dot(box, 4, 5, 24, 3, WX_CLOUD_COLOR, 2);
        wx_dot(box, 8, 11, 18, 3, WX_CLOUD_COLOR, 2);
        wx_dot(box, 4, 17, 24, 3, WX_CLOUD_COLOR, 2);
        return;
    }

    if (code >= 95) {
        /* 雷阵雨 */
        wx_cloud_top(box);
        wx_dot(box, 10, 15, 4, 4, WX_SUN_COLOR, 1);
        wx_dot(box, 15, 18, 4, 4, WX_SUN_COLOR, 1);
        wx_dot(box, 20, 15, 4, 4, WX_SUN_COLOR, 1);
        return;
    }

    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        /* 雪 */
        wx_cloud_top(box);
        wx_dot(box, 9, 17, 3, 3, MD3_ON_SURF, LV_RADIUS_CIRCLE);
        wx_dot(box, 16, 19, 3, 3, MD3_ON_SURF, LV_RADIUS_CIRCLE);
        wx_dot(box, 23, 17, 3, 3, MD3_ON_SURF, LV_RADIUS_CIRCLE);
        return;
    }

    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        /* 雨 */
        wx_cloud_top(box);
        wx_dot(box, 9, 16, 2, 6, WX_RAIN_COLOR, 1);
        wx_dot(box, 16, 18, 2, 6, WX_RAIN_COLOR, 1);
        wx_dot(box, 23, 16, 2, 6, WX_RAIN_COLOR, 1);
        return;
    }

    /* 多云 / 阴 / 未知 */
    wx_cloud_mid(box);
}

static const char *weekday_cn(int wday)
{
    static const char *names[7] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    return names[((wday % 7) + 7) % 7];
}

/* -------------------------------------------------------------------------
 * 下拉手势：在顶部品牌卡上向下拖动超过 60px，弹出 WiFi 面板
 * （工程未开启 LV_USE_GESTURE_RECOGNITION，所以用按下/移动事件手工判断）
 * ------------------------------------------------------------------------- */
static lv_point_t s_press_point;
static bool s_pull_tracking;

static void brand_pull_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        if (!indev) {
            return;
        }
        lv_indev_get_point(indev, &s_press_point);
        s_pull_tracking = (s_press_point.y < 220);
    } else if (code == LV_EVENT_PRESSING && s_pull_tracking) {
        if (!indev) {
            return;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        if (p.y - s_press_point.y > 60) {
            s_pull_tracking = false;
            ui_wifi_open();
        }
    }
}

/* 品牌卡上的 WLAN chip：点击也能打开面板 */
static void wifi_chip_cb(lv_event_t *e)
{
    (void)e;
    ui_wifi_open();
}

/* -------------------------------------------------------------------------
 * 定时器：每秒刷新一次仪表盘数据（LVGL 的定时器由 lv_timer_handler 驱动，
 * 不需要自己开线程）
 * ------------------------------------------------------------------------- */
static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    // 网络时间（SNTP 未同步前显示占位符）
    struct tm tm_info;
    if (wifi_mgr_get_localtime(&tm_info)) {
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M:%S", &tm_info);
        lv_label_set_text(s_time_label, buf);
    } else {
        lv_label_set_text(s_time_label, "--:--:--");
    }

    // 天气卡（武进区）：大温度 + 描述 + 3 天预报
    static char last_temp[8] = "";
    static char last_desc[24] = "";
    static int last_code = -1;
    static int last_fc_code[3] = { -1, -1, -1 };
    static char last_fc_text[3][16] = { "", "", "" };
    static int last_wday[3] = { -1, -1, -1 };
    weather_info_t wi;

    struct tm tm_now;
    int today_wday = 0;
    if (wifi_mgr_get_localtime(&tm_now)) {
        today_wday = tm_now.tm_wday;
    }
    for (int i = 0; i < 3; i++) {
        int wd = (today_wday + 1 + i) % 7;
        if (wd != last_wday[i]) {
            last_wday[i] = wd;
            lv_label_set_text(s_wx_fc_day[i], (i == 0) ? "明天" : weekday_cn(wd));
        }
    }

    if (weather_mgr_get(&wi)) {
        char buf[24];

        snprintf(buf, sizeof(buf), "%d°", wi.temp_c);
        if (strcmp(buf, last_temp) != 0) {
            strcpy(last_temp, buf);
            lv_label_set_text(s_wx_temp, buf);
        }

        snprintf(buf, sizeof(buf), "%s  %d%%", wi.desc, wi.humidity);
        if (strcmp(buf, last_desc) != 0) {
            strcpy(last_desc, buf);
            lv_label_set_text(s_wx_desc, buf);
        }

        if (wi.code != last_code) {
            last_code = wi.code;
            wx_set_icon(s_wx_big_icon, wi.code);
        }

        for (int i = 0; i < 3; i++) {
            if (wi.daily[i].code != last_fc_code[i]) {
                last_fc_code[i] = wi.daily[i].code;
                wx_set_icon(s_wx_fc_icon[i], wi.daily[i].code);
            }
            snprintf(buf, sizeof(buf), "%d°/%d°", wi.daily[i].tmax, wi.daily[i].tmin);
            if (strcmp(buf, last_fc_text[i]) != 0) {
                strcpy(last_fc_text[i], buf);
                lv_label_set_text(s_wx_fc_text[i], buf);
            }
        }
    }

    // 贵金属卡：黄金 / 白银（人民币/克 + 美元/盎司）
    static char last_gold_main[16] = "";
    static char last_gold_sub[20] = "";
    static char last_silver_main[16] = "";
    static char last_silver_sub[20] = "";
    metals_info_t mi;
    if (metals_mgr_get(&mi)) {
        char buf[24];

        snprintf(buf, sizeof(buf), "%.2f", mi.gold_cny_per_gram);
        if (strcmp(buf, last_gold_main) != 0) {
            strcpy(last_gold_main, buf);
            lv_label_set_text(s_gold_price, buf);
        }

        snprintf(buf, sizeof(buf), "$%.2f/oz", mi.gold_usd_per_oz);
        if (strcmp(buf, last_gold_sub) != 0) {
            strcpy(last_gold_sub, buf);
            lv_label_set_text(s_gold_sub, buf);
        }

        snprintf(buf, sizeof(buf), "%.2f", mi.silver_cny_per_gram);
        if (strcmp(buf, last_silver_main) != 0) {
            strcpy(last_silver_main, buf);
            lv_label_set_text(s_silver_price, buf);
        }

        snprintf(buf, sizeof(buf), "$%.2f/oz", mi.silver_usd_per_oz);
        if (strcmp(buf, last_silver_sub) != 0) {
            strcpy(last_silver_sub, buf);
            lv_label_set_text(s_silver_sub, buf);
        }
    }

    // 油价卡：92# / 95#（元/升）
    static char last_92[12] = "";
    static char last_95[12] = "";
    oil_info_t oi;
    if (oil_mgr_get(&oi)) {
        char buf[12];

        snprintf(buf, sizeof(buf), "%.2f", oi.p92);
        if (strcmp(buf, last_92) != 0) {
            strcpy(last_92, buf);
            lv_label_set_text(s_oil_92, buf);
        }

        snprintf(buf, sizeof(buf), "%.2f", oi.p95);
        if (strcmp(buf, last_95) != 0) {
            strcpy(last_95, buf);
            lv_label_set_text(s_oil_95, buf);
        }
    }

    // 品牌卡上的 WLAN 状态 chip
    static wifi_mgr_state_t last_state = (wifi_mgr_state_t)-1;
    wifi_mgr_state_t st = wifi_mgr_get_state();
    if (st != last_state) {
        last_state = st;
        const char *text = "WLAN OFF";
        uint32_t color = MD3_ON_SURF_V;
        if (st == WIFI_MGR_CONNECTED) {
            text = "WLAN ON";
            color = MD3_PRIMARY;
        } else if (st == WIFI_MGR_CONNECTING) {
            text = "WLAN ...";
        } else if (st == WIFI_MGR_FAILED) {
            text = "WLAN ERR";
        }
        lv_label_set_text(s_wifi_chip_label, text);
        lv_obj_set_style_text_color(s_wifi_chip_label, lv_color_hex(color), 0);
    }
}

/* -------------------------------------------------------------------------
 * 贵金属行情卡（全宽，黄金 + 白银共用一个圆角矩形）
 * 每行：圆点 + 名称 + 人民币/克大字 + 单位 …… 右侧美元/盎司小字
 * ------------------------------------------------------------------------- */
static void metals_row(lv_obj_t *card, const char *name, uint32_t dot_color,
                       uint32_t price_color, lv_obj_t **price_out, lv_obj_t **sub_out)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 54);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    // 金属圆点标识
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    make_label(row, name, &font_cjk_28, MD3_ON_SURF_V);
    *price_out = make_label(row, "--.--", &lv_font_montserrat_36, price_color);
    make_label(row, "元/克", &font_cjk_28, MD3_ON_SURF_V);

    lv_obj_t *sp = lv_obj_create(row);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 0, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

    *sub_out = make_label(row, "$--.--/oz", &lv_font_montserrat_14, MD3_ON_SURF);
}

static void build_metals_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, LV_PCT(100), 128);
    lv_obj_set_style_pad_hor(card, 14, 0);
    lv_obj_set_style_pad_ver(card, 6, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(MD3_CARD_HIGH), 0);  // 高一层 elevation
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x3A2A55), 0);  // 轻微紫色渐变，更鲜活
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    metals_row(card, "黄金", 0xFFD166, MD3_PRIMARY, &s_gold_price, &s_gold_sub);
    metals_row(card, "白银", 0xDCE3F0, MD3_SECONDARY, &s_silver_price, &s_silver_sub);
}

/* -------------------------------------------------------------------------
 * 油价卡（中国石化 92# / 95#，元/升）
 * ------------------------------------------------------------------------- */
static lv_obj_t *oil_row(lv_obj_t *card, const char *tag)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 40);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    make_label(row, tag, &lv_font_montserrat_14, MD3_ON_SURF_V);
    lv_obj_t *price = make_label(row, "--.--", &lv_font_montserrat_26, MD3_PRIMARY);
    make_label(row, "元/升", &font_cjk_20, MD3_ON_SURF_V);

    return price;
}

static void build_oil_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 186, 160);
    lv_obj_set_style_pad_hor(card, 10, 0);
    lv_obj_set_style_pad_ver(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 标题：红色圆点 + 中国石化
    lv_obj_t *head = lv_obj_create(card);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_height(head, 34);
    lv_obj_set_style_pad_column(head, 6, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *dot = lv_obj_create(head);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    make_label(head, "中国石化", &font_cjk_20, MD3_ON_SURF);

    s_oil_92 = oil_row(card, "92#");
    s_oil_95 = oil_row(card, "95#");
}

/* -------------------------------------------------------------------------
 * 天气卡（常州市武进区），压缩成右下角窄卡片
 * 布局按参考图：顶部定位点 + 城市 + 右侧天气图标，中间大温度，下方 3 天预报
 * 配色沿用 MD3 Dark，不引入新主题
 * ------------------------------------------------------------------------- */
static void build_weather_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_card(parent, 232, 160);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // 顶行：定位点 + 城市名 + 右侧天气图标
    lv_obj_t *top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, 24);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 6, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *pin = lv_obj_create(top);
    lv_obj_remove_style_all(pin);
    lv_obj_set_size(pin, 10, 10);
    lv_obj_set_style_bg_color(pin, lv_color_hex(MD3_PRIMARY), 0);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(pin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(pin, LV_OBJ_FLAG_SCROLLABLE);

    // 城市文案：来自保存的定位（默认常州 武进），网页可修改
    char loc[48] = "常州 武进";
    weather_mgr_get_location(NULL, NULL, loc, sizeof(loc));
    s_wx_city_label = make_label(top, loc, &dash_motto_font, MD3_ON_SURF);

    lv_obj_t *sp = lv_obj_create(top);
    lv_obj_remove_style_all(sp);
    lv_obj_set_size(sp, 0, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_SCROLLABLE);

    s_wx_big_icon = lv_obj_create(top);
    lv_obj_remove_style_all(s_wx_big_icon);
    lv_obj_set_size(s_wx_big_icon, 32, 24);
    lv_obj_remove_flag(s_wx_big_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_wx_big_icon, LV_OBJ_FLAG_SCROLLABLE);
    wx_set_icon(s_wx_big_icon, 3);

    // 大温度 + 描述/湿度
    lv_obj_t *temp_row = lv_obj_create(card);
    lv_obj_remove_style_all(temp_row);
    lv_obj_set_width(temp_row, LV_PCT(100));
    lv_obj_set_height(temp_row, 42);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(temp_row, 10, 0);
    lv_obj_remove_flag(temp_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(temp_row, LV_OBJ_FLAG_CLICKABLE);

    s_wx_temp = make_label(temp_row, "--°", &lv_font_montserrat_36, MD3_ON_SURF);
    s_wx_desc = make_label(temp_row, "--", &lv_font_montserrat_14, MD3_ON_SURF_V);

    // 预报：明天 / 周X + 图标 + 最高/最低
    for (int i = 0; i < 3; i++) {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 24);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

        s_wx_fc_day[i] = make_label(row, (i == 0) ? "明天" : "周-",
                                    &lv_font_source_han_sans_sc_16_cjk, MD3_ON_SURF_V);

        s_wx_fc_icon[i] = lv_obj_create(row);
        lv_obj_remove_style_all(s_wx_fc_icon[i]);
        lv_obj_set_size(s_wx_fc_icon[i], 32, 24);
        lv_obj_remove_flag(s_wx_fc_icon[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(s_wx_fc_icon[i], LV_OBJ_FLAG_SCROLLABLE);
        wx_set_icon(s_wx_fc_icon[i], 3);

        s_wx_fc_text[i] = make_label(row, "--°/--°", &lv_font_montserrat_14, MD3_ON_SURF);
    }
}

/* -------------------------------------------------------------------------
 * 搭建 UI：对象树
 *   screen
 *    ├─ brand      （品牌 + 时间 + WLAN 状态）
 *    ├─ row        （三张统计卡片，Flex 横向等分）
 *    └─ weather    （武进区天气：大温度 + 3 天预报）
 * 根对象用 Flex 纵向排列，子卡片只关心自己的高度即可。
 * ------------------------------------------------------------------------- */
static void create_dashboard_ui(void)
{
    // 大字库：3755 个 GB2312 一级常用字（20px 4bpp），缺字回退到内置思源黑体。
    // 网页可自由输入常用汉字，web_config.c 用同一字体做字形校验。
    dash_motto_font = font_cjk_20_full;
    dash_motto_font.fallback = &lv_font_source_han_sans_sc_16_cjk;

    lv_obj_t *scr = lv_screen_active();

    // MD3 Surface Level 0 深紫底，边距 16px，卡片间距 12px
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(MD3_SCR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 10, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---------- 1. 品牌卡 ----------
    lv_obj_t *brand = lv_obj_create(scr);
    lv_obj_remove_style_all(brand);
    lv_obj_set_width(brand, LV_PCT(100));
    lv_obj_set_height(brand, 120);
    lv_obj_set_style_bg_color(brand, lv_color_hex(MD3_CARD), 0);
    lv_obj_set_style_bg_opa(brand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(brand, 0, 0);
    lv_obj_set_style_radius(brand, 16, 0);
    lv_obj_set_style_pad_hor(brand, 12, 0);
    lv_obj_set_style_pad_ver(brand, 6, 0);
    lv_obj_set_style_pad_column(brand, 18, 0);
    lv_obj_set_flex_flow(brand, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brand, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(brand, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(brand, LV_OBJ_FLAG_CLICKABLE);
    // 下拉手势入口
    lv_obj_add_event_cb(brand, brand_pull_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(brand, brand_pull_cb, LV_EVENT_PRESSING, NULL);

    // logo 圆角容器（透明底，仅裁剪）
    lv_obj_t *logo_wrap = lv_obj_create(brand);
    lv_obj_remove_style_all(logo_wrap);
    lv_obj_set_size(logo_wrap, 108, 108);
    lv_obj_set_style_bg_opa(logo_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(logo_wrap, 24, 0);                       // MD3 shape scale Extra Large
    lv_obj_set_style_border_width(logo_wrap, 0, 0);
    lv_obj_set_style_pad_all(logo_wrap, 0, 0);
    lv_obj_set_style_clip_corner(logo_wrap, true, 0);
    lv_obj_set_flex_flow(logo_wrap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(logo_wrap, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(logo_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(logo_wrap, LV_OBJ_FLAG_CLICKABLE);           // 让按下事件落到品牌卡上

    s_logo_img = lv_image_create(logo_wrap);
    lv_image_set_src(s_logo_img, web_config_get_logo_src());   // 可能是网页上传的自定义头像
    lv_obj_set_size(s_logo_img, 108, 108);

    lv_obj_t *brand_col = lv_obj_create(brand);
    lv_obj_remove_style_all(brand_col);
    lv_obj_set_size(brand_col, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_flex_flow(brand_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(brand_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(brand_col, 4, 0);
    lv_obj_remove_flag(brand_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(brand_col, LV_OBJ_FLAG_CLICKABLE);           // 同上

    make_label(brand_col, "TIME", &lv_font_montserrat_14, MD3_ON_SURF_V);
    s_time_label = make_label(brand_col, "--:--:--", &lv_font_montserrat_26, MD3_ON_SURF);

    // WLAN 状态 chip（点击打开联网面板）
    lv_obj_t *wifi_chip = lv_obj_create(brand_col);
    lv_obj_remove_style_all(wifi_chip);
    lv_obj_set_height(wifi_chip, 26);
    lv_obj_set_width(wifi_chip, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(wifi_chip, lv_color_hex(MD3_CARD_HIGH), 0);
    lv_obj_set_style_bg_opa(wifi_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_chip, 13, 0);
    lv_obj_set_style_border_width(wifi_chip, 0, 0);
    lv_obj_set_style_pad_hor(wifi_chip, 10, 0);
    lv_obj_set_style_pad_ver(wifi_chip, 0, 0);
    lv_obj_set_flex_flow(wifi_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(wifi_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wifi_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wifi_chip, wifi_chip_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_chip_label = make_label(wifi_chip, "WLAN OFF", &lv_font_montserrat_14, MD3_ON_SURF_V);

    // 右上角箴言：贴在品牌卡右侧靠下区域
    lv_obj_t *motto_wrap = lv_obj_create(brand);
    lv_obj_remove_style_all(motto_wrap);
    lv_obj_set_height(motto_wrap, LV_PCT(100));
    lv_obj_set_flex_grow(motto_wrap, 1);
    lv_obj_set_flex_flow(motto_wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(motto_wrap, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(motto_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(motto_wrap, LV_OBJ_FLAG_CLICKABLE);           // 按下事件落到品牌卡上

    s_motto_label = make_label(motto_wrap, web_config_get_motto(), &dash_motto_font, MD3_TERTIARY);
    lv_obj_set_width(s_motto_label, LV_PCT(100));                   // 允许换行，不撑破品牌卡
    lv_obj_set_style_text_align(s_motto_label, LV_TEXT_ALIGN_RIGHT, 0);

    // ---------- 2. 贵金属行情卡（黄金 + 白银同一个圆角矩形） ----------
    build_metals_card(scr);

    // ---------- 3. 底部：左油价卡 + 右天气卡 ----------
    lv_obj_t *bottom = lv_obj_create(scr);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, 160);
    lv_obj_set_style_pad_column(bottom, 10, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    build_oil_card(bottom);
    build_weather_card(bottom);

    // 定时器：每秒刷新一次数据
    lv_timer_create(ui_timer_cb, 1000, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 + CO5300 (QSPI) + CST820 + 自定义 LVGL9 仪表盘");

    // 1. QSPI 总线初始化
    spi_bus_config_t bus_config = {
        .sclk_io_num = EXAMPLE_PIN_NUM_LCD_SCK,
        .data0_io_num = EXAMPLE_PIN_NUM_LCD_D0,
        .data1_io_num = EXAMPLE_PIN_NUM_LCD_D1,
        .data2_io_num = EXAMPLE_PIN_NUM_LCD_D2,
        .data3_io_num = EXAMPLE_PIN_NUM_LCD_D3,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(EXAMPLE_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    // 2. LCD Panel IO (QSPI)
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    esp_lcd_panel_io_handle_t panel_io = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_LCD_HOST, &io_config, &panel_io));

    // 3. CO5300 Panel 初始化
    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        // IDF 6.x 把 rgb_endian 改名为 rgb_ele_order；IDF 5.x 请写 .rgb_endian
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };

    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel_handle));
    s_panel = panel_handle;

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, EXAMPLE_LCD_X_GAP, EXAMPLE_LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_co5300_set_brightness(panel_handle, 80));

    ESP_LOGI(TAG, "CO5300 QSPI 显示初始化完成 (%dx%d)", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);

    // 4. CST820 触摸初始化（I2C）
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
        .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_TOUCH_RST,
        .int_gpio_num = EXAMPLE_TOUCH_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t touch_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst820(tp_io_handle, &tp_cfg, &touch_handle));

    ESP_LOGI(TAG, "CST820 触控初始化完成");

    // 5. esp_lvgl_adapter 初始化
    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg =
        ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
            panel_handle,
            panel_io,
            EXAMPLE_LCD_H_RES,
            EXAMPLE_LCD_V_RES,
            ESP_LV_ADAPTER_ROTATE_0,
            EXAMPLE_PIN_NUM_LCD_TE,
            EXAMPLE_LCD_PIXEL_CLOCK_HZ,
            4,
            16);

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(disp != NULL);

    esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, touch_handle);
    lv_indev_t *indev = esp_lv_adapter_register_touch(&touch_cfg);
    assert(indev != NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    // 6. WiFi（STA）+ SNTP 对时
    wifi_mgr_init();
    wifi_mgr_scan_start();   // 开机先扫一次，面板首次打开就有列表

    // 6.0 读取网页配置（自定义头像 / 箴言），必须在创建 UI 之前
    web_config_storage_init();

    // 6.1 在线天气（常州市武进区，联网并校时后自动拉取）
    weather_mgr_init();

    // 6.2 贵金属实时行情（黄金 + 白银，美元/盎司 → 人民币/克）
    metals_mgr_init();

    // 6.3 油价（中国石化 92#/95#，江苏）
    oil_mgr_init();

    // 7. 创建 UI（LVGL 对象只能在其任务上下文中操作，必须加锁）
    if (esp_lv_adapter_lock(portMAX_DELAY) == ESP_OK) {
        create_dashboard_ui();
        ui_wifi_init();
        esp_lv_adapter_unlock();
    }

    // 8. 网页设置服务（浏览器修改左上角头像与右上角箴言）
    web_config_server_start();

    ESP_LOGI(TAG, "自定义仪表盘已启动");
}
