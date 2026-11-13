/*
 * WiFi 下拉面板 UI（Android 通知栏风格）
 *
 * 结构（全部挂在 lv_layer_top() 上，盖在仪表盘之上）：
 *   overlay（半透明遮罩，点击空白处关闭）
 *    ├─ panel（从顶部滑下的圆角面板：标题 / SCAN / 状态 / SSID 列表）
 *    └─ pwd_page（密码输入页：SSID + 密码框 + 键盘 + 取消/连接）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 创建界面对象并启动刷新定时器。必须在持有 LVGL 锁的上下文里调用。 */
void ui_wifi_init(void);

/* 打开 WiFi 面板（下拉手势或点击状态 chip 时调用）。 */
void ui_wifi_open(void);

#ifdef __cplusplus
}
#endif
