# SideLantern 无线副屏接口

iPad 用户在选定主机后主动选择「只连主屏」「只连副屏」或「扩展模式」。打开客户端可以查询模式状态，只有用户选定模式才会请求串流。普通 Moonlight 客户端继续使用 GameStream 的应用选择流程。

- `primary` 串流 Windows 当前主显示器。
- `secondary` 串流目标副屏，允许 Windows 临时停用主屏，结束后恢复原布局。活动显示器只能包含主屏和目标副屏，目标副屏也可以在连接时才启用。
- `extend` 串流目标副屏。目标已扩展时直接使用，尚未启用时由主机启用并加入原有布局。校验原有活动显示器的设置后开始串流。

## 客户端协议

使用配对后已有的 GameStream HTTPS 端口、客户端证书和服务端证书校验。`GET /sidelantern/v1/stream-modes` 返回 GameStream XML，根节点有 `status_code="200"` 和 `version="1"`。每个 `mode` 节点有 `id`、`available`（`1` 或 `0`）和 `reason`。可用时原因是空字符串。旧主机可以继续使用普通桌面串流。

开始桌面串流时，iPad 选用 `/applist` 中的 `Desktop` 条目，并在现有 `/launch` 或 `/resume` 请求的参数中追加 `sidelantern_mode=primary|secondary|extend`。主机再次检查模式和显示器状态。成功时沿用现有的 RTSP 会话地址。拒绝时 `status_code` 为非 200，`sidelantern_reason` 为稳定的原因代码。发现接口只描述当前状态，串流请求本身决定是否能够开始。

首版共享一条采集管线，三个模式需要采集空闲，已有串流继续运行。主屏或副屏目标在会话内按稳定 ID 固定，显示器断开时结束会话。临时布局由会话持有，申请失败、协商超时或会话结束时恢复。失败原因包括 `unknown_mode`、`capture_busy`、`primary_display_unavailable`、`secondary_display_unavailable`、`multiple_secondary_displays`、`other_displays_active`、`display_preparation_failed` 和 `unsupported_platform`。

副屏模式优先使用主机明确选定的非主屏目标。没有此类配置时，通过 Windows 显示设备的硬件 ID 识别 MttVDD，自动选取唯一可用的输出。驱动已提供可用输出但输出未启用时，连接时可自动加入 Windows 扩展布局。驱动尚未提供输出时，需先在驱动控制面板启用一个。若出现多个可用虚拟输出，需在主机选定其中一个。处于镜像布局的输出需先改为独立输出。`primary` 始终解析当前主显示器。收到 `capture_busy` 时，客户端应保留当前会话并提示用户稍后再试。
