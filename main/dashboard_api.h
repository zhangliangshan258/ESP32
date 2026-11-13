/*
 * 仪表盘 UI 的对外操作接口
 *
 * 供 Web 配置模块（web_config.c）在运行时修改屏幕上呈现的内容。
 * 下面两个函数不是线程安全的，调用方必须持有 LVGL 适配层锁
 * （esp_lv_adapter_lock / unlock）。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 箴言默认文案（与 LVGL 字体子集/内置字库均已覆盖） */
#define DASH_DEFAULT_MOTTO "相信相信的力量!"

/* 箴言标签使用的字体（font_cjk_20_full：3755 个 GB2312 一级常用字，
 * 再以 LVGL 内置思源黑体 16px 兜底），由 main.c 在创建 UI 时初始化，
 * Web 端用它校验汉字是否有字形。 */
extern lv_font_t dash_motto_font;

/* 修改右上角箴言文字（UTF-8） */
void dash_set_motto_text(const char *utf8);

/* 修改天气卡左上角的城市文案（如 "常州 武进"） */
void dash_set_weather_city(const char *utf8);

/* 替换左上角头像图片源（lv_image_dsc_t* 或文件路径） */
void dash_set_logo_src(const void *src);

#ifdef __cplusplus
}
#endif
