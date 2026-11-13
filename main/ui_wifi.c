/*
 * WiFi 下拉面板 UI 实现（Material Design 3 Dark）
 */

#include "ui_wifi.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "lvgl.h"

#include "wifi_manager.h"

/* ---------------- 局部调色板（与仪表盘保持一致） ---------------- */
#define C_SCR_BG     0x121018
#define C_PANEL      0x211F26
#define C_ITEM       0x2B2930
#define C_ITEM_HI    0x3A3742
#define C_ON_SURF    0xE6E0E9
#define C_ON_SURF_V  0xCAC4D0
#define C_PRIMARY    0xD0BCFF
#define C_OUTLINE    0x938F99
#define C_TERTIARY   0xEFB8C8

#define WIFI_MAX_APS      24
#define WIFI_PANEL_H      400

static lv_obj_t *s_overlay;      /* 半透明遮罩，全屏 */
static lv_obj_t *s_panel;        /* 从顶部滑下的面板 */
static lv_obj_t *s_status;       /* 状态行 */
static lv_obj_t *s_scan_btn;     /* SCAN 按钮 */
static lv_obj_t *s_list;         /* SSID 列表容器 */

static lv_obj_t *s_pwd_page;     /* 密码输入页 */
static lv_obj_t *s_pwd_title;    /* "Connect to XXX" */
static lv_obj_t *s_pwd_ta;       /* 密码输入框 */
static lv_obj_t *s_pwd_err;      /* 密码为空的提示 */
static lv_obj_t *s_kb;           /* 键盘 */

static char s_target_ssid[33];
static char s_item_ssid[WIFI_MAX_APS][33];
static wifi_auth_mode_t s_item_auth[WIFI_MAX_APS];
static uint32_t s_last_gen;
static bool s_built;

/* ---------------- 工具函数 ---------------- */

static void panel_height_anim_cb(void *var, int32_t value)
{
    lv_obj_set_height((lv_obj_t *)var, value);
}

static void animate_panel_height(int32_t from, int32_t to, uint32_t duration,
                                 lv_anim_completed_cb_t done_cb)
{
    lv_anim_delete(s_panel, NULL);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, duration);
    lv_anim_set_exec_cb(&a, panel_height_anim_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done_cb) {
        lv_anim_set_completed_cb(&a, done_cb);
    }
    lv_anim_start(&a);
}

static int rssi_to_level(int rssi)
{
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -78) return 2;
    return 1;
}

static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg,
                           int32_t width, int32_t height)
{
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, width, height);
    lv_obj_set_style_bg_color(pill, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, height / 2, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(pill);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(fg), 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);

    return pill;
}

/* ---------------- 面板开关 ---------------- */

static void update_status(void);
static void refresh_list(void);

static void show_panel_again(void)
{
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);
    }
    lv_obj_add_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

static void panel_hide_done_cb(lv_anim_t *a)
{
    (void)a;
    lv_obj_set_height(s_panel, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void close_panel(void)
{
    if (!s_built || lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    show_panel_again();               /* 键盘收起、密码页关闭 */
    animate_panel_height(lv_obj_get_height(s_panel), 0, 180, panel_hide_done_cb);
}

void ui_wifi_open(void)
{
    if (!s_built) {
        return;
    }

    show_panel_again();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    int32_t h = lv_obj_get_height(s_panel);
    animate_panel_height(h, WIFI_PANEL_H, 220, NULL);

    /* 每次打开都刷新一遍 + 自动扫描 */
    s_last_gen = wifi_mgr_scan_generation();
    refresh_list();
    update_status();
    wifi_mgr_scan_start();
}

/* ---------------- 密码页 ---------------- */

static void open_pwd_page(const char *ssid)
{
    strncpy(s_target_ssid, ssid, sizeof(s_target_ssid) - 1);
    s_target_ssid[sizeof(s_target_ssid) - 1] = '\0';

    lv_label_set_text_fmt(s_pwd_title, "Connect to %s", s_target_ssid);
    lv_textarea_set_text(s_pwd_ta, "");
    lv_obj_add_flag(s_pwd_err, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_kb, s_pwd_ta);
}

static void pwd_cancel_cb(lv_event_t *e)
{
    (void)e;
    show_panel_again();
}

static void pwd_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *pwd = lv_textarea_get_text(s_pwd_ta);
    if (!pwd || pwd[0] == '\0') {
        lv_label_set_text(s_pwd_err, "Password can not be empty");
        lv_obj_remove_flag(s_pwd_err, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    wifi_mgr_connect(s_target_ssid, pwd);
    show_panel_again();
    update_status();
}

/* ---------------- 列表 ---------------- */

static void ap_item_event_cb(lv_event_t *e)
{
    lv_obj_t *item = lv_event_get_target(e);
    uint32_t idx = (uint32_t)(uintptr_t)lv_obj_get_user_data(item);
    if (idx == 0 || idx > WIFI_MAX_APS) {
        return;
    }
    idx--;

    if (s_item_auth[idx] == WIFI_AUTH_OPEN) {
        /* 开放网络直接连 */
        wifi_mgr_connect(s_item_ssid[idx], "");
        update_status();
        return;
    }
    open_pwd_page(s_item_ssid[idx]);
}

static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    close_panel();
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    if (wifi_mgr_scan_start()) {
        lv_label_set_text(s_status, "Scanning for networks...");
    }
}

static void refresh_list(void)
{
    if (!s_list) {
        return;
    }

    /* 扫描结果放静态区，避免 LVGL 任务栈溢出（24 条 AP 记录约 2.4KB） */
    static wifi_ap_record_t aps[WIFI_MAX_APS];
    uint16_t n = wifi_mgr_get_aps(aps, WIFI_MAX_APS);
    const char *connected = wifi_mgr_connected_ssid();

    lv_obj_clean(s_list);

    if (n == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, wifi_mgr_is_scanning() ? "Scanning..." : "No networks found");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(C_ON_SURF_V), 0);
        lv_obj_set_style_pad_top(empty, 12, 0);
        return;
    }

    uint16_t shown = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (ssid[0] == '\0') {
            continue;
        }

        /* 同名 SSID（多 BSSID / Mesh 节点）只保留信号最强的一条，列表已按 RSSI 降序 */
        bool dup = false;
        for (uint16_t k = 0; k < shown; k++) {
            if (strcmp(s_item_ssid[k], ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        strncpy(s_item_ssid[shown], ssid, sizeof(s_item_ssid[shown]) - 1);
        s_item_ssid[shown][sizeof(s_item_ssid[shown]) - 1] = '\0';
        s_item_auth[shown] = aps[i].authmode;

        bool is_connected = connected[0] && strcmp(connected, ssid) == 0;

        lv_obj_t *item = lv_obj_create(s_list);
        lv_obj_remove_style_all(item);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 56);
        lv_obj_set_style_bg_color(item, lv_color_hex(is_connected ? C_ITEM_HI : C_ITEM), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(item, 14, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_hor(item, 14, 0);
        lv_obj_set_style_pad_ver(item, 0, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(item, (void *)(uintptr_t)(shown + 1));
        lv_obj_add_event_cb(item, ap_item_event_cb, LV_EVENT_CLICKED, NULL);

        /* SSID 名称 */
        lv_obj_t *name = lv_label_create(item);
        lv_label_set_text(name, ssid);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 240);
        lv_obj_set_style_text_font(name, &lv_font_source_han_sans_sc_16_cjk, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_ON_SURF), 0);
        lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

        /* 右侧：加密标记 + 信号强度小柱子 */
        lv_obj_t *right = lv_obj_create(item);
        lv_obj_remove_style_all(right);
        lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(right, 8, 0);
        lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);

        if (aps[i].authmode != WIFI_AUTH_OPEN) {
            lv_obj_t *key = lv_label_create(right);
            lv_label_set_text(key, "KEY");
            lv_obj_set_style_text_font(key, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(key, lv_color_hex(C_OUTLINE), 0);
            lv_obj_remove_flag(key, LV_OBJ_FLAG_CLICKABLE);
        }

        lv_obj_t *bars = lv_obj_create(right);
        lv_obj_remove_style_all(bars);
        lv_obj_set_size(bars, 30, 20);
        lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(bars, 3, 0);
        lv_obj_remove_flag(bars, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bars, LV_OBJ_FLAG_CLICKABLE);

        int level = rssi_to_level(aps[i].rssi);
        for (int b = 0; b < 4; b++) {
            lv_obj_t *bar = lv_obj_create(bars);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, 4, 5 + b * 4);
            lv_obj_set_style_bg_color(bar,
                                      lv_color_hex((b + 1) <= level ? C_PRIMARY : C_OUTLINE), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar, 2, 0);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }

        shown++;
    }
}

/* ---------------- 状态刷新 ---------------- */

static void update_status(void)
{
    if (!s_status) {
        return;
    }

    if (wifi_mgr_is_scanning()) {
        lv_label_set_text(s_status, "Scanning for networks...");
        return;
    }

    switch (wifi_mgr_get_state()) {
    case WIFI_MGR_CONNECTED:
        if (wifi_mgr_connected_ssid()[0]) {
            lv_label_set_text_fmt(s_status, "Connected: %s", wifi_mgr_connected_ssid());
        } else {
            lv_label_set_text(s_status, "Connected");
        }
        break;
    case WIFI_MGR_CONNECTING:
        lv_label_set_text(s_status, "Connecting...");
        break;
    case WIFI_MGR_FAILED:
        lv_label_set_text(s_status, "Connection failed, check password");
        break;
    default:
        lv_label_set_text(s_status, "Not connected - tap SCAN to search");
        break;
    }
}

static void wifi_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_built || lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    update_status();

    if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        uint32_t gen = wifi_mgr_scan_generation();
        if (gen != s_last_gen) {
            s_last_gen = gen;
            refresh_list();
        }
    }
}

/* ---------------- 构建界面 ---------------- */

static void build_panel(void)
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_panel = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_width(s_panel, LV_PCT(100));
    lv_obj_set_height(s_panel, 0);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 24, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 16, 0);
    lv_obj_set_style_pad_row(s_panel, 10, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_CLICKABLE);

    /* 顶部小横条（拖拽提示） */
    lv_obj_t *handle = lv_obj_create(s_panel);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 44, 4);
    lv_obj_set_style_bg_color(handle, lv_color_hex(C_OUTLINE), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);

    /* 标题行：WLAN + SCAN */
    lv_obj_t *hdr = lv_obj_create(s_panel);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_width(hdr, LV_PCT(100));
    lv_obj_set_height(hdr, 34);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(hdr);
    lv_label_set_text(title, "WLAN");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_ON_SURF), 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_scan_btn = make_pill(hdr, "SCAN", C_ITEM, C_PRIMARY, 84, 32);
    lv_obj_add_event_cb(s_scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 状态行 */
    s_status = lv_label_create(s_panel);
    lv_label_set_text(s_status, "Not connected - tap SCAN to search");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_status, LV_PCT(100));
    lv_obj_set_style_text_font(s_status, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_ON_SURF_V), 0);
    lv_obj_remove_flag(s_status, LV_OBJ_FLAG_CLICKABLE);

    /* 可滚动列表 */
    s_list = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(s_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(C_PRIMARY), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_40, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_list, 2, LV_PART_SCROLLBAR);
}

static void build_pwd_page(void)
{
    s_pwd_page = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_pwd_page);
    lv_obj_set_size(s_pwd_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_pwd_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pwd_page, 0, 0);
    lv_obj_set_style_pad_all(s_pwd_page, 0, 0);
    lv_obj_remove_flag(s_pwd_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_pwd_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pwd_page, LV_OBJ_FLAG_HIDDEN);

    /* 输入卡片 */
    lv_obj_t *card = lv_obj_create(s_pwd_page);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, 420);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    s_pwd_title = lv_label_create(card);
    lv_label_set_text(s_pwd_title, "Connect to");
    lv_label_set_long_mode(s_pwd_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_pwd_title, LV_PCT(100));
    lv_obj_set_style_text_font(s_pwd_title, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(s_pwd_title, lv_color_hex(C_ON_SURF), 0);
    lv_obj_remove_flag(s_pwd_title, LV_OBJ_FLAG_CLICKABLE);

    s_pwd_ta = lv_textarea_create(card);
    lv_obj_set_width(s_pwd_ta, LV_PCT(100));
    lv_obj_set_height(s_pwd_ta, 56);
    lv_textarea_set_one_line(s_pwd_ta, true);
    lv_textarea_set_password_mode(s_pwd_ta, true);
    lv_textarea_set_placeholder_text(s_pwd_ta, "Password");
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(C_ITEM), 0);
    lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_pwd_ta, 12, 0);
    lv_obj_set_style_border_width(s_pwd_ta, 0, 0);
    lv_obj_set_style_pad_all(s_pwd_ta, 12, 0);
    lv_obj_set_style_text_font(s_pwd_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(C_ON_SURF), 0);
    lv_obj_set_style_text_color(s_pwd_ta, lv_color_hex(C_OUTLINE), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_pwd_ta, lv_color_hex(C_PRIMARY), LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(s_pwd_ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_width(s_pwd_ta, 2, LV_PART_CURSOR);

    s_pwd_err = lv_label_create(card);
    lv_label_set_text(s_pwd_err, "Password can not be empty");
    lv_obj_set_style_text_font(s_pwd_err, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pwd_err, lv_color_hex(C_TERTIARY), 0);
    lv_obj_remove_flag(s_pwd_err, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_pwd_err, LV_OBJ_FLAG_HIDDEN);

    /* 按钮行 */
    lv_obj_t *btns = lv_obj_create(card);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_height(btns, 48);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = make_pill(btns, "CANCEL", C_ITEM, C_ON_SURF_V, 120, 48);
    lv_obj_add_event_cb(cancel, pwd_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *connect = make_pill(btns, "CONNECT", C_PRIMARY, C_SCR_BG, 140, 48);
    lv_obj_add_event_cb(connect, pwd_connect_cb, LV_EVENT_CLICKED, NULL);

    /* 键盘 */
    s_kb = lv_keyboard_create(s_pwd_page);
    lv_obj_set_size(s_kb, LV_PCT(100), 232);
    lv_obj_align(s_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb, 0, 0);
    lv_obj_set_style_radius(s_kb, 0, 0);
    lv_obj_set_style_pad_all(s_kb, 6, 0);
    lv_obj_set_style_pad_row(s_kb, 4, 0);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(C_ITEM), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(C_ON_SURF), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_kb, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(C_PRIMARY), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(C_SCR_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(C_PRIMARY), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(C_SCR_BG), LV_PART_ITEMS | LV_STATE_PRESSED);

    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_popovers(s_kb, false);
    lv_keyboard_set_textarea(s_kb, s_pwd_ta);
}

void ui_wifi_init(void)
{
    if (s_built) {
        return;
    }

    build_panel();
    build_pwd_page();

    lv_timer_create(wifi_ui_timer_cb, 500, NULL);

    s_built = true;
}
