# Windows 11 图标设计与 Agent 生成说明

本套图标采用 Windows 11 应用图标的视觉方向，使用内置 `image_gen` 逐张生成。它们是随行组件自己的标识。微软图片用于观察设计语言，不作为软件的成品图标。下载日期：2026-10-03。

## 微软官方手册

- [应用图标概述](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icons)：选择容易识别、能表达功能的图形。
- [应用图标设计](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-design)：以 48×48 网格组织形状，控制隐喻数量、圆角、渐变和叠层阴影，检查深浅背景。
- [应用图标制作](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction)：解释 PNG、ICO、不同显示位置与尺寸的资源准备方式。
- [Windows 图标体系](https://learn.microsoft.com/en-us/windows/apps/design/iconography/)与 [Segoe Fluent Icons](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font)：应用标识与界面操作符号有不同用途。按钮、返回、关闭等操作优先沿用系统字体图标，不把彩色组件标识当成所有按钮的图标。
- [官方设计资源](https://learn.microsoft.com/en-us/windows/apps/design/downloads/)：提供 Windows UI 设计资源与字体下载入口。

## 从参考图片提取的视觉方向

以下是本项目对已下载图片的观察及生成决策，并非微软逐字规范：

1. 使用正视图和薄的平面叠层。靠形状相交表达层级，避免透视、厚度、鼓起的胶囊和塑料质感。
2. 大色块承担识别；同一色相的轻微明暗变化提供层次。去掉霓虹、玻璃反光、高光边框与强烈材质纹理。
3. 阴影主要出现在两个物体相交处。透明画布上不附加地面阴影、展示底座或统一方形背景。
4. 每张图只保留一个主意象和少量辅助形状。天气是太阳与云，待办是大勾，清单是多行方框，避免功能相近的组件混淆。
5. 用小幅圆角、明确边缘和适量留白统一风格。检查 16/24/32/48 像素以及白色、深灰背景，细节不能成为识别的前提。

## 给生成模型的公共提示词

完整的实际请求保存在同目录 `prompts.json`，每张图另有明确的 Subject。参考图片通过 `referenced_image_paths` 一起提交，不能仅用“Fluent 风格”四个字代替参考。

```text
Create ONE original Windows 11 desktop product icon.
The attached official Microsoft reference board is ONLY a style reference.
Do not reproduce the board, text, grid, existing Microsoft products or logos.
Flat front-facing layered graphic illustration; crisp simple silhouette;
modest corner rounding; predominantly solid colors with gentle two-tone shading.
Small masked shadow only where one flat shape overlaps another.
No external cast shadow, glossy plastic, inflated shapes, glass, reflections,
specular highlights, metallic bevels, white rim, neon glow or isometric perspective.
No text, labels, watermark, pedestal, scene or backplate behind the object.
True transparent alpha background; centered square composition;
subject about 78 percent of canvas; readable at 24 pixels.
One main metaphor and at most one supporting metaphor.
Subject: [用一两句话描述组件功能对应的图形和颜色]
```

Agent 生成新组件图标时，先读取这份说明及 `prompts.json`，查看参考图，再独立生成一张。检查结果是否出现厚玻璃、装饰小字、错误符号或白底；有问题就针对该问题重新生成。不要将整张参考板切成组件图标。

## 本地参考文件与来源

文件位于 `references/microsoft/`。除注明外，图片来自上述应用图标设计手册配图。每项均链接实际下载地址。

| 本地文件 | 微软原始下载 |
| --- | --- |
| `abstraction-spectrum.png` | [功能与抽象程度示例](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/abstraction-spectrum.png) |
| `official-app-icon.png` | [应用图标示例](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/iconography_hero_1880.png) |
| `contrast-light-dark.png` | [深浅背景对比](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/contrast-light-dark.png) |
| `layer-and-shadow.png` | [叠层与阴影示意](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/layer-and-shadow.png) |
| `shadow-same-metaphor.png` | [同一意象内的阴影](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/shadow-same-metaphor.png) |
| `shadow-separate-metaphor.png` | [不同意象间的阴影](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/shadow-separate-metaphor.png) |
| `perspective.png` | [透视说明图](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/perspective.png) |
| `icon-design-grid.png` | [设计网格](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/icon-design-grid.png) |
| `icons-aligned-in-grid.png` | [网格对齐示例](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/icons-aligned-in-grid.png) |
| `win-11-icon-locations.png` | [Windows 11 显示位置](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/win-11-icon-locations.png) |
| `winui-gallery-logo.png` | [WinUI Gallery 标识](https://learn.microsoft.com/en-us/windows/includes/images/winui-gallery-logo.png) |
| `iconography-systemicons.svg` | [系统图标示例](https://learn.microsoft.com/en-us/windows/apps/design/iconography/images/iconography_systemicons.svg) |
| `Segoe-Fluent-Icons.zip` | [微软字体资源](https://download.microsoft.com/download/8/f/c/8fc7cbc3-177e-4a22-af48-2a85e1c5bffb/Segoe-Fluent-Icons.zip) |

实际生成主要参考 `abstraction-spectrum.png` 和 `contrast-light-dark.png`。`perspective.png`、`layer-and-shadow.png` 含解释性的透视示意，不能把它们误当成所有应用图标都应使用等距立体的建议。字体 ZIP 仅保存供参考，未安装、未加入程序发布资源。

## 资产接入

- `originals/`：模型原始透明 PNG；`prompts.json`：实际逐图提示词和参考文件。
- 顶层 PNG：256×256；ICO：包含多个 Windows 显示尺寸。
- `build-icons.ps1` 只做尺寸转换和 ICO 编码，保留长宽比与透明度；`preview-icons.ps1` 输出深浅背景及小尺寸总览。
- 初版保存在 `archive/2026-10-02-v1/`，可以直接对比。
- 主程序 EXE、托盘、组件管理列表、卡片标题和组件窗口使用对应资产。用户组件在 `widget.json` 用 `"icon": "icon.png"` 指定包内 PNG。
- 发布脚本只复制顶层成品图标，不复制 `references/`、原图或历史版本。

更严格的像素级小尺寸设计可额外提供专门的 16/24 像素版本。当前 ICO 的各尺寸来自同一模型原图缩放，应通过总览检查实际辨识度，不把自动缩放等同于人工像素校准。
