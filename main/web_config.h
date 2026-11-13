/*
 * 网页配置模块
 *
 * 在设备上启动一个 HTTP 服务，用浏览器即可修改屏幕内容：
 *   GET  /              设置页面
 *   GET  /api/state     当前状态（motto / custom_logo / ssid，纯文本 key=value）
 *   GET  /api/logo.raw  当前头像原始像素（108x108 RGB565 小端，23328 字节）
 *   POST /api/motto     正文为 UTF-8 箴言文本，保存并立即生效
 *   POST /api/logo      正文为 23328 字节 RGB565 小端像素，保存并立即生效
 *   POST /api/weather   正文为 key=value 行（city/district/label/lat/lon），
 *                       坐标由网页在浏览器侧解析，保存后立即刷新天气
 *   DELETE /api/motto   恢复默认箴言
 *   DELETE /api/logo    恢复默认头像
 *   DELETE /api/weather 恢复默认城市（常州 武进）
 *
 * 数据保存在 SPIFFS（storage 分区），掉电不丢。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 挂载 SPIFFS 并读取上次保存的箴言/头像，必须在创建 UI 之前调用 */
void web_config_storage_init(void);

/* 读取当前应显示的箴言（未自定义时返回默认文案） */
const char *web_config_get_motto(void);

/* 读取当前应显示的头像图片源（lv_image_dsc_t*，未自定义时为内置 logo_img） */
const void *web_config_get_logo_src(void);

/* 启动 HTTP 服务，建议在 UI 创建完成后调用 */
void web_config_server_start(void);

#ifdef __cplusplus
}
#endif
