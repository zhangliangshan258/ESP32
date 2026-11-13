/*
 * 中国石化油价（江苏 92# / 95#）
 *
 * 数据源：https://api.qqsuu.cn/api/dm-oilprice?prov=江苏（免 Key）
 * 返回示例：
 *   {"code":200,"msg":"success","data":{"prov":"江苏","p92":"8.85","p95":"9.41",...}}
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;     /* 是否已成功获取过一次 */
    float p92;      /* 92# 汽油：元/升 */
    float p95;      /* 95# 汽油：元/升 */
} oil_info_t;

void oil_mgr_init(void);
bool oil_mgr_get(oil_info_t *out);

#ifdef __cplusplus
}
#endif
