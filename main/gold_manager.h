/*
 * 贵金属实时行情（黄金 / 白银）
 *
 * 后台任务定期拉取国际金银价（gold-api，美元/盎司）与 USD/CNY 汇率，
 * 换算成国内常用的「人民币/克」。UI 通过 metals_mgr_get() 读快照。
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;                 /* 是否已成功获取过一次 */
    float gold_usd_per_oz;      /* 黄金：美元/盎司 */
    float gold_cny_per_gram;    /* 黄金：人民币/克 */
    float silver_usd_per_oz;    /* 白银：美元/盎司 */
    float silver_cny_per_gram;  /* 白银：人民币/克 */
    float usd_cny;              /* 使用的美元兑人民币汇率 */
} metals_info_t;

void metals_mgr_init(void);
bool metals_mgr_get(metals_info_t *out);

#ifdef __cplusplus
}
#endif
