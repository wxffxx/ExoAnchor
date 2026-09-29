# ExoAnchor 品牌资产

当前方向：**11 — Riven Datum**

本目录保存从锁定概念重绘出的确定性生产资产。标志直接沿用
`11-riven-datum.png` 最大独立图形的九块不对称多边形碎片与一个靛蓝验证节点；
所有尺寸复用同一套轮廓和间距。常规横版字标使用项目自有的几何线条；
README 横幅按项目视觉要求使用本地 `HarmonyOS_Sans_Black.ttf` 渲染。

这些文件已经适配当前项目的 Web 固件、GitHub/README、应用图标和硬件丝印
需求，但在正式公开发布前仍应进行相似性与商标检索。

## 项目使用矩阵

| 使用位置 | 当前项目约束 | 推荐资产 |
| --- | --- | --- |
| 固件顶栏 | 现有 CSS 容器为 `34 × 34 px`，深色背景 | `web/exoanchor-ui-mark.svg`；PNG 源使用 `web/ui-mark-128.png`，由 CSS 缩放显示 |
| Agent 悬浮入口 | 现有 CSS 容器为 `30 × 30 px` | `web/exoanchor-ui-mark.svg` 或缩放显示 `web/ui-mark-128.png` |
| 浏览器标签 | 需要兼顾现代 SVG favicon 与传统格式 | `web/favicon.svg`、`web/favicon.ico`（16/32/48） |
| Apple Web Clip | 标准 `180 × 180 px` | `web/apple-touch-icon-180.png` |
| PWA / 安装图标 | 标准 `192 × 192`、`512 × 512`，另需 maskable 安全区 | `web/pwa-icon-192.png`、`web/pwa-icon-512.png`、`web/pwa-icon-maskable-512.png` |
| README / 文档 | GitHub 支持 SVG；宽屏头图需要 PNG 回退 | `svg/exoanchor-lockup-horizontal-color.svg`、`png/exoanchor-readme-banner-1600x400.png` |
| GitHub Social Preview | GitHub 推荐横向社交卡片 | `png/exoanchor-social-preview-1280x640.png` |
| 外壳、贴纸、印刷 | 需要无损缩放和单色版本 | `svg/exoanchor-symbol-color.svg`、`svg/exoanchor-symbol-mono.svg` |
| PCB 丝印 / EDA 导入 | 必须纯单色、无渐变、无字体；毫米尺寸可预测 | `hardware/exoanchor-symbol-silkscreen.svg`；10 mm / 20 mm 的 SVG 与 DXF |

ESP32-P4 固件已接入 `web/exoanchor-ui-mark.svg` 和 `web/favicon.svg`：
生产副本位于固件 `main/www/assets/`，通过 ESP-IDF `EMBED_TXTFILES` 嵌入，
并由独立 HTTP 路由提供给共享顶栏、Agent 浮窗、Agent 空白会话、统一登录框
和所有页面 `<head>`。ICO、Apple Web Clip 与 PWA 图标继续保留为后续安装型
Web 应用入口的准备资产。

## 主文件

### SVG

- `svg/exoanchor-symbol-color.svg`：彩色主图形，透明背景。
- `svg/exoanchor-symbol-mono.svg`：纯单色图形。
- `svg/exoanchor-symbol-reverse.svg`：深色背景用反白图形。
- `svg/exoanchor-symbol-small.svg`：与主图形同轮廓的小尺寸 SVG。
- `svg/exoanchor-wordmark-mono.svg`：自包含字标。
- `svg/exoanchor-lockup-horizontal-*.svg`：横版组合。
- `svg/exoanchor-lockup-stacked-*.svg`：竖版组合。

### PNG / WebP

- 图形 PNG：128、256、512、1024 px；不保存低于 128 px 的独立 PNG。
- 横版 PNG：600、1200、2400 px。
- WebP：256 px 图形与 1200 px 横版无损版本。
- `brand-assets-preview.png`：整套资产快速检查图。

### 硬件

- `hardware/exoanchor-symbol-silkscreen.svg`：可任意缩放的单色母版。
- `hardware/*-10mm.svg|dxf`：适合小型板面与外壳标识。
- `hardware/*-20mm.svg|dxf`：适合主板丝印、治具或较大的设备铭牌。

DXF 使用毫米单位和闭合多段线。不同 EDA 对 DXF 填充的支持不同；需要实心
丝印时优先导入 SVG，或在 EDA 中把 DXF 闭合轮廓转换为实心区域。

## 颜色

| Token | 色值 | 用途 |
| --- | --- | --- |
| Ink | `#111318` | 浅色背景主图形与字标 |
| Indigo | `#5966D8` | 验证节点 |
| Indigo Light | `#7480F0` | 深色背景验证节点 |
| Deep Indigo | `#0C1022` | 应用图标和社交卡片底色 |
| Off White | `#F2F3F0` | 预览与浅色展示背景 |
| White | `#F7F9FC` | 反白图形 |

靛蓝只用于中心验证节点，不应用于状态成功、警告或故障语义；固件现有绿色、
黄色、红色状态色继续承担交互语义。

## 最小尺寸

- 所有独立 PNG 图标源文件不小于 `128 × 128 px`，界面通过 CSS 缩放显示。
- 完整彩色图形：数字界面显示尺寸建议不小于 `48 px`。
- 小尺寸 SVG：可显示为 `16–34 px`，优先使用 `symbol-small` 或
  `web/exoanchor-ui-mark.svg`。
- 横版组合：数字界面建议不小于 `240 px` 宽。
- 印刷图形：建议不小于 `8 mm` 宽。
- PCB 丝印：建议不小于 `10 mm` 宽，并在具体工厂规则下检查最小线宽和间距。

不要在小尺寸中自行合并碎片、删除靛蓝节点、改变碎片次序或把图形封装进新的
盾牌、圆环或六边形。

`favicon.ico` 是唯一的低分辨率例外：ICO 文件内部保留浏览器规范所需的
16/32/48 px 帧，同时提供 `web/favicon-source-256.png` 作为高分辨率源图。

## 重新生成

所有导出文件由一个确定性脚本生成：

```bash
python3 scripts/build_brand_assets.py
```

脚本依赖 Pillow。生成 README 横幅时，本机需安装 HarmonyOS Sans Black，也可以
通过 `EXOANCHOR_WORDMARK_FONT` 指定字体文件。字体文件不随本仓库分发，其文件名
和 SHA-256 会写入 `manifest.json`；生成后的尺寸、模式和文件大小也记录在该文件。
