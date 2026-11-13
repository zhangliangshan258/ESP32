/*
 * 在线天气（常州市武进区）
 *
 * 联网并完成 SNTP 对时后，后台任务定期从 Open-Meteo 拉取实时天气，
 * UI 通过 weather_mgr_get() 读快照。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int code;           /* WMO 天气代码 */
    int tmax;           /* 最高温 °C */
    int tmin;           /* 最低温 °C */
} weather_day_t;

typedef struct {
    bool valid;         /* 是否已成功获取过一次 */
    int temp_c;         /* 温度（整数摄氏度） */
    int humidity;       /* 相对湿度 % */
    int code;           /* WMO 天气代码 */
    char desc[16];      /* 英文短描述，如 CLOUD */
    weather_day_t daily[3];   /* 明天、后天、大后天 */
} weather_info_t;

void weather_mgr_init(void);
bool weather_mgr_get(weather_info_t *out);

/* 修改定位（网页设置的城市/区域），lat/lon 为 WGS84 十进制度。
 * 可在 weather_mgr_init() 之前调用（开机读取保存的配置），
 * 运行中调用会立即唤醒后台任务重新拉取天气。 */
void weather_mgr_set_location(double lat, double lon, const char *label);

/* 读取当前定位；label 为屏幕显示的城市文案（如 "常州 武进"） */
bool weather_mgr_get_location(double *lat, double *lon, char *label, size_t label_size);

#ifdef __cplusplus
}
#endif
