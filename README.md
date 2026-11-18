# kwifi

Windows 10/11 上的热点工具：一键开热点、按设备限速、优先级分档、隐身连接模式。

## 下载

到 [Releases](https://github.com/halfkite/kwifi/releases) 下载 `kwifi-N.N.N.exe`
（N.N.N 是版本号），放到任意文件夹双击运行。

控制热点需要管理员权限，第一次运行会弹 UAC，点「是」即可。

## 功能

| 功能 | 说明 |
|---|---|
| 开/关/重启热点 | WinRT `NetworkOperatorTetheringManager`，失败回落承载网络 + ICS |
| 设备列表 | ARP + 反向 DNS + NetBIOS，在线/历史设备都能看到 |
| 改名 / 备注 | 按 MAC 保存，离线也保留 |
| 单设备限速 | 每个方向一个开关 + Mbps 输入，数据面令牌桶执行 |
| 优先级 P1–P5 | 数字越小越优先，总带宽吃满时只放行高优先级 |
| 本机优先级 | 单独一档，与客户端同场比较 |
| 封禁 / 黑名单 | 数据面双向丢弃，封禁设备常驻黑名单卡片 |
| 隐身模式 | 屏蔽 IPv6、组播探测、TTL 归一 |
| 开机自启（托盘） | 注册表 `HKCU\...\Run`；被拦截时自动改用「启动」文件夹快捷方式 |
| 开机自启热点 | 登录后自动开启热点，不用点任何东西 |
| 后台服务 | 可选，装一次后热点在后台运行，界面不再需要管理员 |
| 主题 | 默认浅色，右上角可切深色 |

### 后台服务（消除反复弹 UAC）

默认情况下热点由界面进程自己启动，所以点「开启热点」时会弹一次 UAC。装成服务后就不
用了：

- 在「程序设置 → 后台服务」点「安装服务」，弹一次 UAC，点「是」
- 之后服务开机自动启动（LocalSystem），热点、限速、封禁都在后台完成
- 界面以普通权限连接它，**之后再也不弹 UAC**

命令行等价操作（需要管理员）：

```
kwifi.exe --install-service     安装并启动
kwifi.exe --service-status      查看状态
kwifi.exe --uninstall-service   卸载
```

服务名 `kwifi`，显示名 `kwifi hotspot service`，可在 `services.msc` 里管理。服务的配置
和日志在 `%ProgramData%\Kwifi\data`（所有用户共用）；安装时会把当前用户的
`config.json`、`devices.json` 迁过去。

卸载服务后自动回到便携模式。

### 开机自启的两种模式

两个开关互相独立：

- **开机自启**：登录后只驻留托盘，不开热点，**不弹 UAC**
- **开机自启热点**：登录后自动开热点。开热点必须管理员权限，所以 Windows 会在
  登录时弹一次 UAC 授权框（点「是」即可，只需一次）

两个都关时，程序只能手动启动。

## 代码签名

免费代码签名由 [SignPath.io](https://about.signpath.io) 提供，证书由
[SignPath Foundation](https://signpath.org) 签出 —— 申请中，尚未生效，当前版本未签名。

- Approvers：仓库所有者（每次签名请求需人工批准）
- 隐私：本程序不会向你未指定的联网系统传输任何信息。热点名、密码、设备名、限速都只
  保存在本地 `%LOCALAPPDATA%\Kwifi\data`，从不上传
- 所有二进制均由 `.github/workflows/build.yml` 从本仓库源码构建，没有仓库外产出的文件
## 数据位置

```
%LOCALAPPDATA%\Kwifi\data\      便携模式
%ProgramData%\Kwifi\data\       装了后台服务后（所有用户共用）
```

`config.json`（热点名、密码、带宽策略、自启开关）、`devices.json`（设备名、备注、
限速、黑名单）、`server.log`、`kwifi.log`。卸载程序直接删 exe 和这个目录即可。

## 已知限制

- 控制热点必须管理员权限；拒绝 UAC 时程序仍驻留托盘，但无法开关热点
- 「开机自启热点」在登录时会弹一次 UAC（Windows 的硬性要求，无法静默提权）
- 隐身只处理 TTL / 组播 / IPv6 这类常见特征，不保证绕过所有检测手段
- 仅对自己拥有或被授权的网络使用