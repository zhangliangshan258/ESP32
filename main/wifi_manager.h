/*
 * WiFi 连接管理器：扫描 / 连接 / SNTP 对时
 *
 * 只做“STA 模式 + 扫描结果缓存 + 联网 + 网络时间”这几件事，
 * UI 层通过轮询下面的接口来刷新界面，不直接接触 esp_wifi 的事件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_IDLE = 0,      // 未连接
    WIFI_MGR_CONNECTING,    // 正在连接 / 等待 IP
    WIFI_MGR_CONNECTED,     // 已联网（已拿到 IP）
    WIFI_MGR_FAILED,        // 最近一次连接失败（密码错误 / 超时等）
} wifi_mgr_state_t;

/* 初始化 NVS / netif / event loop / WiFi（STA）。重复调用无副作用。 */
void wifi_mgr_init(void);

/* 触发一次异步扫描，结果通过 wifi_mgr_scan_generation() 的版本号变化通知 UI。 */
bool wifi_mgr_scan_start(void);
bool wifi_mgr_is_scanning(void);

/* 拷贝扫描结果（按信号强度从强到弱排序），返回实际条数。 */
uint16_t wifi_mgr_get_aps(wifi_ap_record_t *out, uint16_t max_count);

/* 每完成一次扫描 +1，UI 用它判断是否需要重建列表。 */
uint32_t wifi_mgr_scan_generation(void);

wifi_mgr_state_t wifi_mgr_get_state(void);

/* 连接指定 AP。password 可为 NULL / ""（开放网络）。 */
bool wifi_mgr_connect(const char *ssid, const char *password);

/* 当前已连接 AP 的 SSID，未连接返回空字符串。 */
const char *wifi_mgr_connected_ssid(void);

/* 系统时间是否已由 SNTP 同步（以 2023 年为界判断）。 */
bool wifi_mgr_time_synced(void);

/* 取本地时间（时区 CST-8），未对时返回 false。 */
bool wifi_mgr_get_localtime(struct tm *out);

#ifdef __cplusplus
}
#endif
