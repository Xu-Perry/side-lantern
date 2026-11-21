# SideLantern

SideLantern 是基于 [Sunshine](https://github.com/SunshineStream/Sunshine) 的 Windows x64 串流主机，通过独立的 Electron 桌面界面管理原生采集和串流引擎。使用原版 Moonlight 即可配对，默认串流 Windows 主显示器。

## 当前功能

- 在首页处理 Moonlight PIN 配对请求、查看活跃会话和已配对设备，并解除单台设备的配对。多台客户端可以同时观看同一个串流来源。
- 在串流设置中选择显示器、编码器、码率上限、HEVC、AV1 和主机声音。当前可选用已在 Windows 中扩展、且引擎能识别的 MttVDD 虚拟屏。
- 关闭窗口或最小化时保留托盘和后台串流。通过托盘完全退出时停止串流服务。界面支持简体中文和英文，主题可跟随系统或手动选择浅色、深色。
- Windows 安装包内置独立服务，默认勾选 ViGEmBus 支持，并复用兼容的已安装版本。可选的加密迁移支持保留 Sunshine 主机身份、配对记录和应用条目。

## 构建与试用

- 开发模式使用独立配置和测试端口，可以与原 Sunshine 同时运行。启动和测试命令见 [桌面端开发说明](desktop/README.md)。
- 安装包使用本仓库构建的 Sunshine 引擎。构建和安装步骤见 [Windows 安装包说明](desktop/installer/README.md)。
- 安装版与原 Sunshine 使用同一组默认串流端口。连接 SideLantern 前先临时停止原 Sunshine，试用后可恢复原服务并继续使用原配置和配对。

## 验收进度

- 两份原版 Moonlight 在本机隔离环境中同时收到桌面视频。解除其中一份的配对后，另一份保持串流。本机也已验证安装、普通用户启动服务、正常退出、强制结束和卸载，并确认已安装的 ViGEmBus 被复用。
- 下一轮用局域网内另一台设备验证主屏画面、声音、键鼠和手柄，并复测配对、托盘保活和退出断流。
- 后续实现虚拟副屏的自动创建、模式切换和退出清理，再为自定义 Moonlight 客户端添加副屏连接入口。正式发行还需要完成安装包签名。
- Windows 主机已提供双向文件传输接口，iPad 客户端待接入。见 [文件传输设计和接口文档](docs/superpowers/specs/2026-11-21-side-lantern-file-transfer.md)。

项目许可证见 [LICENSE](LICENSE)。
