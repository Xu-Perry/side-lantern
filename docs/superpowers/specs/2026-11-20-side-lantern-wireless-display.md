# SideLantern 无线副屏接口

iPad 用户在选定主机后主动选择「只连主屏」「只连副屏」或「扩展模式」。打开客户端可以查询模式状态，只有用户选定模式才会请求串流。普通 Moonlight 客户端继续使用 GameStream 的应用选择流程。

- `primary` 串流 Windows 当前主显示器。
- `secondary` 串流目标副屏，允许 Windows 临时停用主屏，结束后恢复原布局。活动显示器只能包含主屏和目标副屏，目标副屏也可以在连接时才启用。
- `extend` 串流目标副屏。目标已扩展时直接使用，尚未启用时由主机启用并加入原有布局。校验原有活动显示器的设置后开始串流。

## 客户端协议

使用配对后已有的 GameStream HTTPS 端口、客户端证书和服务端证书校验。`GET /sidelantern/v1/stream-modes` 返回 GameStream XML，根节点有 `status_code="200"` 和 `version="1"`。每个 `mode` 节点有 `id`、`available`（`1` 或 `0`）和 `reason`。可用时原因是空字符串。旧主机可以继续使用普通桌面串流。

开始桌面串流时，iPad 选用 `/applist` 中的 `Desktop` 条目，并在现有 `/launch` 或 `/resume` 请求的参数中追加 `sidelantern_mode=primary|secondary|extend`。主机再次检查模式和显示器状态。成功时沿用现有的 RTSP 会话地址。拒绝时 `status_code` 为非 200，`sidelantern_reason` 为稳定的原因代码。发现接口只描述当前状态，串流请求本身决定是否能够开始。

第一台设备决定采集哪块屏幕。后续设备通过 `/resume` 加入正在运行的应用，共享当前采集画面，也可使用普通 Moonlight 入口。新请求带上的选屏模式只在启动第一路串流时生效。模式发现返回 `join_current="1"` 时，客户端可显示「加入当前串流」。显式选定的显示器按稳定 ID 固定。若主机为串流临时改了显示布局，所有共享该画面的会话都会持有布局，最后一路结束后恢复。首路申请失败或协商超时也会恢复布局。

副屏模式优先使用主机明确选定的非主屏目标。没有此类配置时，通过 Windows 显示设备的硬件 ID 识别 MttVDD，自动选取唯一可用的输出。驱动已提供可用输出但输出未启用时，连接时可自动加入 Windows 扩展布局。驱动尚未提供输出时，需先在驱动控制面板启用一个。若出现多个可用虚拟输出，需在主机选定其中一个。处于镜像布局的输出需先改为独立输出。首路串流选择 `primary` 时解析当前主显示器。失败原因包括 `unknown_mode`、`primary_display_unavailable`、`secondary_display_unavailable`、`multiple_secondary_displays`、`other_displays_active`、`display_preparation_failed` 和 `unsupported_platform`。

串流期间在 Windows「显示设置」修改 MttVDD 的桌面分辨率后，主机会在正常断开或退出时按虚拟屏 ID 保存。下次临时启用该屏时，主机尝试应用保存的分辨率。虚拟屏已经启用时，使用它当前的桌面分辨率。若驱动不再支持保存的分辨率，这次会话使用 Windows 选出的模式，原偏好保留到用户再次手动修改。这个偏好独立于串流画面的编码分辨率。

## 串流中的手动画质设置

Windows 主机在模式发现结果中提供 `manual_video_version="1"` 时，能够接收这个协议。客户端须能处理串流中的新编码尺寸和关键帧，并在显式模式的 `/launch` 或 `/resume` 请求中附加 `sidelantern_manual_video=1`。实际可用性取决于会话建立时选中的编码器。

- `GET /sidelantern/v1/manual-video` 用同一配对证书查询已连接的会话，返回 XML 中的 `session_id`、`width`、`height`、`fps`、`bitrate_kbps` 和 `status`。
- `POST /sidelantern/v1/manual-video` 在查询参数中提交同一个 `session_id` 和四个整数设置。`bitrate_kbps` 是视频编码器的目标码率。若只改一项，从查询结果复制其余设置。主机的分辨率和码率上限同时适用。
- XML 中的 `status_code="202"` 表示已接收请求。随后轮询查询接口，`pending` 表示正在处理，`applied` 表示已采用返回的设置，`rejected` 表示编码器初始化失败，返回值仍是原设置。主机同一时间处理一项修改。`409` 表示该会话 ID 已过期或已有修改在处理。

更改只作用于当前会话的视频。主机保留连接、音频、编码格式、HDR 协商、采集目标和 Windows 显示布局。编码器可在同一连接中重建并输出新关键帧。客户端按关键帧更新播放尺寸和触控坐标，网络波动不会自行切换参数。

例如，当前会话 ID 为 `42`，编码尺寸为 `1280×720`，帧率为 `60` 时，可以把视频目标码率改到 `5500 Kbps`。

```http
POST /sidelantern/v1/manual-video?session_id=42&width=1280&height=720&fps=60&bitrate_kbps=5500
```

客户端以 XML 根节点的 `status_code` 判断请求结果。`404` 表示当前没有该配对设备可调整的会话。收到 `applied` 后，以查询结果更新设置界面。尺寸变更时，在收到新关键帧和格式参数后更新播放比例与触控映射。
