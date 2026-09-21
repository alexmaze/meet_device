# 征辰 minicam / 太空舱 硬件手册

状态：真机已点亮（Meet 固件 0.3.0）  
板级代码：`main/boards/zhengchen-minicam/`  
厂商资料：`../esp32s3/太空舱资料包/资料包/`（xiaozhi 2.2.6 + 独有板型）  
对照：不要抄官方 xiaozhi 的 `zhengchen-cam`（那是 ES8311+ES7210，引脚不同）

产品形态名 **太空舱**，工程板型名 **zhengchen-minicam**，Meet 领域词 **CompanionDevice**。

## SoC 与存储

| 项 | 值 |
| --- | --- |
| 芯片 | ESP32-S3 QFN56，rev v0.2，双核 + LP Core，240 MHz |
| Flash | 16 MB，QIO |
| PSRAM | 片上 8 MB octal，80 MHz，`AP_3v3` |
| 手头这台 MAC | `3c:dc:75:fe:7f:80` |

分区（`partitions.csv`，16 MB）：

| 名 | 偏移 | 大小 |
| --- | --- | --- |
| nvs | `0x9000` | 24 KB |
| otadata | `0xF000` | 8 KB |
| phy_init | `0x11000` | 4 KB |
| ota_0 | `0x20000` | 5 MB |
| ota_1 | `0x520000` | 5 MB |
| model（esp-sr） | `0xA20000` | 1.5 MB |
| storage | `0xBA0000` | 4.375 MB |

内部 RAM 很紧：Wi-Fi 必须在 AFE 之前初始化；`SPIRAM_MALLOC_ALWAYSINTERNAL=2048`、`RESERVE_INTERNAL=98304`；Wi-Fi RX 缓冲按太空舱收紧（static RX 3 / dynamic RX 6 / BA win 3）。同时开两个带 wakenet 的 AFE 会 mmap 冲突并重启。

## 烧录

- 原生 USB-Serial/JTAG，Mac 口典型为 `/dev/cu.usbmodem11401`。
- **Boot 键是 GPIO11，不是 strapping 的 GPIO0**，按 Boot 不能进下载模式。插上 USB 后 `idf.py` 可直接复位下载。
- 资料包另有 CH340 路径和 `烧录工具/`（Flash Download Tool、串口驱动、`烧录教程.png`）。手头这台走原生 USB，不必装 CH340。
- 环境：ESP-IDF **6.1**（`~/.espressif/v6.1/esp-idf`）。
- 命令：`idf.py -p /dev/cu.usbmodem11401 flash`
- 开启了 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`：首次成功连上 Meet 后端后才应标 valid。
- 厂商合并包：`资料包/备用固件（黑表情包）/merged-binary.bin`。

## 屏幕（ST7789 SPI）

物理面板 240×320。设备**横放**使用（逻辑 320×240）。

| 信号 | GPIO |
| --- | --- |
| MOSI | 41 |
| CLK | 46 |
| DC | 39 |
| CS | 40 |
| RST | NC（走软件复位） |
| 背光 | 42，输出反向 |

SPI3、40 MHz、RGB、颜色反相。刷屏前要把 LVGL 的 RGB565 字节对调（ST7789 SPI 要大端，否则抗锯齿文字会出彩边）。Meet 横屏 MADCTL（已用真机校正）：

- `SWAP_XY=true`，`MIRROR_X=false`，`MIRROR_Y=true`

不要直接套小智 `DISPLAY_*_1`（MX=true, MY=false）。小智横屏是「硬件转屏 + LVGL 再转 90°」；Meet 只走硬件，那套组合会 **180° 倒立**。

中文用 `main/ui/fonts/font_meet_cjk_16_4.c`（GB2312 一级 + 界面用字）。不要用 LVGL 自带 `source_han_sans_sc_16_cjk`，缺简体字会出方框。

## 音频（ES8388）

I2C 与摄像头共用：SDA=GPIO1，SCL=GPIO2，地址 `0x20`。无独立 PA 脚（`GPIO_NC`）。

| I2S | GPIO |
| --- | --- |
| MCLK | 38 |
| WS | 13 |
| BCLK | 14 |
| DIN（麦） | 12 |
| DOUT（喇叭） | 45 |

输入带 AEC 参考声（双声道）。厂商小智固件 codec 跑 24 kHz；Meet 当前 **16 kHz 原生** 对齐 AFE / 上行，下行 24 kHz PCM 再抗混叠到 16 kHz。若 ES8388 在 16 kHz 失败，再退回 24 kHz。

唤醒词配置为 **嗨乐鑫**（`model` 分区）。当前启动跳过 Wake AFE，避免和第二路 VC AFE 抢 flash mmap；P0 靠 Boot 键通话。

## 按键、电量、状态线

| 功能 | 硬件 |
| --- | --- |
| Boot | GPIO11，上拉，低有效。单击接通/挂断；长按约 3 s 进出设置 |
| 音量 | ADC GPIO9（`ADC_CHANNEL_8`）。0–700 mV 减，900–3000 mV 加 |
| 电池 | ADC GPIO3；参考 ADC GPIO4（标称 1270 mV） |
| 充电检测 | `ref_raw > 2300` 视为充电；充电时电压按 ×2 估算，否则用参考校正。满电约 4.2 V，3.4–4.2 V 映射 0–100% |
| 通话状态线 | GPIO47：通话=0，空闲=1（给外壳灯/外部 MCU） |
| 板载 LED | 资料包 GPIO48；Meet 固件未用 |

## 摄像头（P0 关闭）

DVP，XCLK 24 MHz，I2C 与 ES8388 同总线。PWDN/RESET 均为 NC。

| 功能 | GPIO |
| --- | --- |
| D0–D7 | 7, 43, 5, 6, 8, 44, 10, 15 |
| XCLK | 17 |
| PCLK | 16 |
| VSYNC | 21 |
| HREF | 18 |
| SIOD/SIOC | 1 / 2（共用音频 I2C） |

第一版语音陪伴不开摄像头，避免和 I2C / PSRAM 抢资源。

## 固件侧不要踩的坑

1. 板级只认 `zhengchen-minicam`，不要用 `zhengchen-cam`。
2. 先 Wi-Fi 后 AFE；不要启动时同时建两路带 wakenet 的 AFE。
3. 横屏镜像以本文 MADCTL 为准，不以小智 `_1` 宏为准。
4. 烧录不靠 Boot 键进下载模式。
5. 资料包源码默认 I2S 24 kHz；Meet 已改 16 kHz，改采样率要连 codec、AFE、上行一起看。
