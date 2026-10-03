# 随行组件图标

使用内置 `image_gen` 图像生成工具制作的统一 Fluent 风格图标。每个图标独立生成，透明背景；生成提示词完整保存在 `prompts.json`，原始输出保存在 `originals/`。

| 文件名 | 用途 |
| --- | --- |
| `app` | 主程序、控制中心、托盘、通用原生组件 |
| `weather` | 官方天气 |
| `todo` | 官方待办 |
| `system` | 官方系统状态 |
| `quick` | 官方常用入口 |
| `bluetooth-battery` | 独立蓝牙电量 MOD |
| `note` / `text` / `checklist` / `links` | 用户组件对应类型 |

每个图标提供 256×256 透明 PNG 及 ICO。ICO 包含 16、20、24、32、40、48、64、128、256 像素共九个分辨率。编码时保留生成的透明度和长宽比，不拉伸或重新绘制原图。

```powershell
./assets/icons/build-icons.ps1
./assets/icons/preview-icons.ps1
```

`preview.png` 便于检查深浅背景和小尺寸显示。主程序图标通过 `AppResources.rc` 嵌入 EXE，运行时资源由构建脚本复制到 `icons/`。控制中心、卡片标题及组件窗口使用同一套标识；当前蓝牙包另带 `icon.png`，无需依赖安装路径。

组件可在 `widget.json` 声明 `"icon": "icon.png"`。该文件必须是包内已有的 PNG，相对路径不能越界，最多 4 MiB；零代码导入会一并复制声明的图标。原生包按原有规则复制整个目录。此字段控制列表和卡片标题图标；窗口 ICO 按宿主内置类型提供。未声明时自动使用对应类型图标，通用原生组件使用主程序标识。
