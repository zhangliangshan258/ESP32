# ESP32-S3 桌面仪表盘

基于 **ESP32-S3 + CO5300 QSPI AMOLED + LVGL 9** 的桌面信息仪表盘，
显示天气、黄金/白银、油价、实时时钟，并提供一个内置网页远程配置屏幕内容。

---

## 硬件

| 模块 | 说明 |
|---|---|
| MCU | ESP32-S3-WROOM-1 / ESP32-S3-WROOM-2 |
| 屏幕 | CO5300 4.6" AMOLED（QSPI，460×460，带 TE 信号） |
| 触摸 | CST820（I²C） |
| 电源 | 3.3 V，推荐 500 mA 以上 |

引脚分配见 `main/main.c` 顶部宏定义，可按需修改。

---

## 固件

| 项目 | 值 |
|---|---|
| ESP-IDF | ≥ 5.5（当前基于 IDF 6.1 验证） |
| LVGL | 9.4 |
| esp_lvgl_adapter | 0.4 |
| esp_lcd_co5300 | 2.0 |
| 分区 | factory 6 MB + spiffs 256 KB |

---

## 功能

### 仪表盘 UI

- **品牌卡**：左上角头像（108×108 RGB565）+ 右上角箴言，默认显示出厂内容
- **时钟卡**：本地时间（SNTP 自动对时）
- **天气卡**：Open-Meteo 免 Key 接口，当前温度 + 天气描述 + 湿度 + 3 天预报
- **贵金属卡**：黄金 / 白银 人民币/克 + 美元/盎司
- **油价卡**：92# / 95# 当前价格
- **WiFi 下拉面板**：在品牌卡区域向下拖动超过 60 px 弹出 WiFi 管理面板

### 内置设置网页

设备启动后在局域网暴露 HTTP 服务，用浏览器打开 `http://<设备IP>/` 即可：

- **头像上传**：支持 JPEG / PNG / WebP，浏览器端自动裁剪为 108×108 RGB565 原始像素，无需设备端图片解码器
- **箴言修改**：最多 12 字，保存后立即生效并写入 SPIFFS
- **天气城市**：浏览器端先通过 Photon（优先）/ Open-Meteo（兜底）地理编码解析中文城市名，再把坐标提交给设备，屏幕天气数据立即刷新
- **状态接口**：`GET /api/state` 返回 `key=value` 文本，包含箴言、城市、设备 IP 等

### 持久化

- `factory` 分区：固件
- `spiffs` 分区（256 KB）：
  - `/spiffs/logo.bin` — 自定义头像像素
  - `/spiffs/motto.txt` — 自定义箴言
  - `/spiffs/weather.txt` — 自定义城市 + 坐标

开机自动加载，修改通过 HTTP 实时生效并写入 Flash。

---

## 项目结构

```
.
├── CMakeLists.txt
├── partitions.csv              # 自定义分区表（含 storage/SPIFFS）
├── sdkconfig                  # 本次构建生成的配置（不入库）
├── sdkconfig.defaults         # 默认 Kconfig（入库）
├── managed_components/        # ESP-IDF 组件管理器拉取的依赖
├── components/                # 可选：用户自建组件
├── build/                     # 构建产物（不入库）
└── main/
    ├── CMakeLists.txt         # 把 web/index.html 嵌入为 _binary_index_html
    ├── idf_component.yml      # 组件依赖声明
    ├── main.c                 # app_main + UI 构建 + 引脚定义
    ├── co5300_init_cmds.h     # CO5300 AMOLED 初始化命令序列
    ├── dashboard_api.h        # 对外暴露的 UI 更新函数（dash_set_*）
    ├── wifi_manager.c/h       # WiFi 连接管理
    ├── ui_wifi.c/h            # WiFi 面板 UI
    ├── weather_manager.c/h    # Open-Meteo 天气客户端
    ├── gold_manager.c/h       # 贵金属行情
    ├── oil_manager.c/h        # 油价行情
    ├── web_config.c/h         # HTTP 服务 + SPIFFS 存储 + REST 接口
    ├── font_cjk_20.c          # 自定义 20 px CJK 子集（15 字）
    ├── font_cjk_20_full.c     # GB2312 一级常用字 3755 个，20 px 4 bpp
    ├── font_cjk_28.c          # 28 px CJK 子集
    ├── logo_img.c             # 默认头像（108×108 RGB565）
    └── web/index.html         # 设置网页源文件
```

---

## 编译与烧录

```powershell
# 1. 设置 ESP-IDF 环境（Windows PowerShell）
$env:IDF_PATH = "C:\esp\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
. "$env:IDF_PATH\export.ps1"

# 2. 进入工程目录
cd <本项目路径>

# 3. 配置目标芯片
idf.py set-target esp32s3

# 4. 编译
idf.py build

# 5. 烧录（COM8 为实际串口号）
idf.py -p COM8 flash monitor
```

首次构建时组件管理器会自动从 `dependencies.lock` 下载 `lvgl/lvgl`、`esp_lvgl_adapter`、
`esp_lcd_co5300` 并缓存到 `managed_components/`。

---

## 串口日志

```
I (8155) WIFI_MGR: WiFi 已连接，IP 192.168.31.107
I (8160) WEB_CFG: 存储已就绪，箴言="我思故我在"，自定义头像=0，天气城市=常州 新北(31.8100,119.9700)
I (8200) HTTPD: Starting HTTP server on port 80
I (8300) WEATHER: 当前天气: 16°C DRIZZLE 湿度 76% (code 51)
```

---

## 默认 PIN（QFN-28 封装 CO5300，QSPI + TE）

| ESP32-S3 | 连接 |
|---|---|
| GPIO 9 | LCD CS |
| GPIO 12 | LCD SCK |
| GPIO 13 / 14 / 15 / 16 | LCD QSPI D0/D1/D2/D3 |
| GPIO 10 | LCD RST |
| GPIO 11 | LCD TE |
| GPIO 40 | I²C SCL（触摸） |
| GPIO 39 | I²C SDA（触摸） |
| GPIO 41 | 触摸 RST |
| GPIO 42 | 触摸 INT |

如需切换到其他屏幕或触摸，修改 `main/main.c` 顶部的引脚宏即可。

---

## 远程访问（可选）

当前设置页仅在局域网可用。要在外网（比如手机流量）操作设备，可选用以下任一方案：

1. **内网穿透（推荐）**：家里常开电脑 / NAS / 树莓派跑 `cloudflared tunnel --url http://<ESP32-IP>:80`，
   设备固件零改动即可获得 HTTPS 公网地址。
2. **MQTT 中转**：设备主动连接免费 Broker（EMQX / 巴法云），另做一个公网网页发指令，
   需给固件加 MQTT 客户端。
3. **路由器端口映射 + DDNS**：要求家宽有公网 IPv4（多数家庭为 CGNAT，无独立公网 IP）。

> ⚠️ 暴露到公网前务必给 HTTP 接口加鉴权（请求头带 `X-Token`，设备端校验）。当前版本无鉴权。

---

## 第三方依赖

| 组件 | License |
|---|---|
| ESP-IDF | Apache 2.0 |
| LVGL | MIT |
| esp_lvgl_adapter | Apache 2.0 |
| esp_lcd_co5300 | Apache 2.0 |
| CO5300 init commands | OSPTEK 提供，保留在 `co5300_init_cmds.h` |

---

## License

项目自身代码按 **CC-BY-4.0** 发布；使用第三方组件请同时遵守其各自 License。
