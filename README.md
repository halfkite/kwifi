# kwifi

Windows 10/11 上的热点控制台：一键开热点、按设备限速、优先级分档、隐身模式（降低被校园网识别为共享的概率）。

**单文件 1 MB，不需要安装 Node.js 或任何运行时。** 双击 `kwifi.exe` 即可。

## 体积对比

| 版本 | 体积 | 依赖 |
|---|---|---|
| 旧版（Node 后端 + 打包 Node 运行时） | 90.7 MB | 自带 90 MB node.exe |
| **当前（原生 C 后端）** | **1.0 MB** | 无（仅系统自带的 WebView2） |

后端从 Node.js 重写为原生 C（`native/server.c`），界面、`net.ps1`、数据面 `ttlfix.exe`、WebView2 加载器全部内嵌进 exe，首次运行按需解包到数据目录。

## 运行

双击 `kwifi.exe` → 弹 UAC（控制热点必须管理员权限）→ 出现界面窗口。

- 界面是**自己的窗口**（WebView2 控件渲染），不是浏览器标签页
- 右下角托盘常驻，关窗只是收起，右键菜单：打开界面 / 重启热点 / 停止热点 / 打开日志目录 / 退出
- 托盘退出时界面窗口一起关闭

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
| **开机自启热点** | 登录后自动开启热点，不用点任何东西 |
| 主题 | 默认浅色，右上角可切深色 |

### 开机自启的两种模式

两个开关互相独立：

- **开机自启**：登录后只驻留托盘，不开热点，**不弹 UAC**
- **开机自启热点**：登录后自动开热点。这个模式必须管理员权限，所以 Windows
  会在登录时弹一次 UAC 授权框（点「是」即可，只需一次）

如果两个都关，程序只能手动启动。

写入位置：
```
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
  Kwifi = "C:\path\to\kwifi.exe" --tray
```
若注册表被安全软件拦截，会自动退到启动文件夹快捷方式
`%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\kwifi.lnk`。

### 关于「未知发布者」提示

第一次运行会看到两类提示，来源不同：

**0. 先看这个：现在打开程序默认不弹 UAC 了**

以前双击就弹 UAC，现在只有真正需要管理员权限的动作才会请求（开启热点、封禁、
限速、TTL、隐身）。看设备、改名字、改设置全程不弹。

还想让「开机自启」也完全不弹？装一次免 UAC 任务：

```
kwifi.exe --install-task
```

之后每次都以管理员身份启动且**没有任何弹窗**（连「未知发布者」都看不到，因为没有
对话框）。卸载：`kwifi.exe --remove-task`。

**1. UAC 的「未知发布者」**

Windows 对**没有数字签名**的程序一律这么显示。配置改不了，只有签名能解决。

- 只想在自己这台机器上消掉：`powershell -ExecutionPolicy Bypass -File tools\sign.ps1`
  （自签名 + 本机信任，**只在装过证书的电脑生效**）
- 想让**所有人**下载后都不再看到：需要 CA 签发的证书，见
  **[docs/SIGNING.md](docs/SIGNING.md)** —— 免费走 SignPath Foundation，付费走
  Certum / Sectigo 的个人 OV 证书（约 ¥600/年）

**2. SmartScreen 的「Windows 已保护你的电脑」**

只在从网上下载过（文件带 Internet 标记）时出现，和签名无关：

```powershell
Unblock-File .\kwifi.exe
```

或右键 exe → 属性 → 勾选「解除锁定」。

签名状态可以自己核对：

```powershell
powershell -ExecutionPolicy Bypass -File tools\verify-signature.ps1
```
### 程序身份

exe 现在带完整的版本资源、图标和清单，任务管理器和「应用和功能」里显示为
`kwifi` / `kwifi hotspot console`，不再是空白或 anonymous。

## 目录

```
kwifi.exe            单文件程序（托盘 + 界面 + HTTP 后端）
native/              原生源码
  kwifi.c            托盘、窗口、注册表、提权、进程管理
  server.c           HTTP/SSE 后端、配置与设备存储
  kgwebview.cpp      WebView2 窗口宿主
  ttlfix.c           数据面（WinDivert：TTL 归一、限速、优先级、封禁）
  respatch.c         给 exe 写入图标/版本/清单
  app.rc app.manifest app.ico
  gen-embedded.ps1   把 UI 与脚本转成 C 数组
  gen-icon.ps1       生成多尺寸图标
  build.ps1          一键编译
  wv2shim/           WebView2 头文件（MinGW 缺 EventToken.h 的补丁）
ps/net.ps1           网控脚本（状态/开关/限速/封禁/隐身）
ui/                  前端（原生 HTML/CSS/JS）
tools/sign.ps1       自签名证书 + 签名（只对装过证书的机器生效）
tools/verify-signature.ps1   查看签名状态与 SHA256
docs/SIGNING.md      如何拿到「所有人信任」的签名
docs/PUBLISH.md      发布到 GitHub 的步骤
.github/workflows/   CI：构建、签名、发布
```

## 数据位置

```
%LOCALAPPDATA%\Kwifi\data\        单文件版
<exe 目录>\data\                  绿色版 / 开发目录
```

`config.json`（热点名、密码、带宽策略、自启开关）、`devices.json`（设备名、备注、
限速、黑名单）、`server.log`、`kwifi.log`、`ttlfix.conf`、`ttlfix-stats.json`、
`webview\`（界面缓存）。

升级版本不会覆盖 `data` 目录。

## 重新编译

```
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

需要 [zig](https://ziglang.org/download/) 在 PATH（自带 C/C++ 编译器和链接器）。
脚本会：

1. 生成 `native\embedded.h`（把 UI、`net.ps1`、`ttlfix.exe`、WebView2 加载器转成字节数组）
2. 编译 `kwifi.c`、`server.c`、`kgwebview.cpp`，链接成 `kwifi.exe`
3. 用 `respatch.exe` 写入图标、版本信息和清单
4. 把 PE 头改成 GUI 子系统（否则双击会弹黑框）

## 数据面

`ttlfix.exe` 通过 WinDivert 在 IP 层处理 IPv4：

1. 出站且目标不在 `192.168.137.0/24` → TTL 改成 128
2. 目标是客户端 → 下载方向计数/整形
3. 来源是客户端 → 上传方向计数/整形
4. 总带宽吃满时按优先级丢弃低优先级流量
5. 封禁的客户端双向丢弃

配置由后端每秒热更新（行式文本）：

```
enabled=1
ttl=128
hostprio=3
hostip=10.74.139.5
total=75000
block 192.168.137.9
limit 192.168.137.5 up=512 down=2048 prio=2
```

首次启用会安装 WinDivert 的签名内核驱动（管理员权限），杀软可能提示。

## 已知限制

- 控制热点必须管理员权限；拒绝 UAC 时程序仍会驻留托盘但无法开关热点
- 「开机自启热点」在登录时会弹一次 UAC（Windows 的硬性要求，无法静默提权）
- 自签名证书只在装过该证书的电脑上消除「未知发布者」
- 隐身只处理 TTL / 组播 / IPv6 这类常见特征，不保证绕过所有检测手段
- 仅对自己拥有或被授权的网络使用

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org) — **applied for, not yet active**. Builds
published before approval are unsigned; every release ships a `SHA256SUMS.txt` so the
binary can be verified regardless.

- Committers and reviewers: repository maintainers
- Approvers: repository owners (each signing request is approved manually)
- Privacy policy: this program does not transfer any information to other networked
  systems unless specifically requested by the user. Hotspot names, passwords, device
  names and speed limits are stored locally under `%LOCALAPPDATA%\Kwifi\data` and are
  never uploaded.
- All binaries are built from this repository by `.github/workflows/build.yml`; no
  binary is produced outside CI.

If you sign with your own certificate instead, replace the first paragraph with your
certificate details and keep the rest.

## License

MIT — see [LICENSE](LICENSE).