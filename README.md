# kwifi

Windows 10/11 上的热点工具：一键开热点、按设备限速、优先级分档、隐身连接模式。

**单个 `kwifi.exe`，约 1 MB，不需要安装 Node.js 或任何运行时。** 下载后双击即可。

## 下载

到 [Releases](https://github.com/halfkite/kwifi/releases) 下载 `kwifi-N.N.N.exe`
（N.N.N 是版本号）。单文件程序，放到任意文件夹运行，不用安装，也没有安装程序。

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
| 主题 | 默认浅色，右上角可切深色 |

### 开机自启的两种模式

两个开关互相独立：

- **开机自启**：登录后只驻留托盘，不开热点，**不弹 UAC**
- **开机自启热点**：登录后自动开热点。开热点必须管理员权限，所以 Windows 会在
  登录时弹一次 UAC 授权框（点「是」即可，只需一次）

两个都关时，程序只能手动启动。

## 数据位置

```
%LOCALAPPDATA%\Kwifi\data\
```

`config.json`（热点名、密码、带宽策略、自启开关）、`devices.json`（设备名、备注、
限速、黑名单）、`server.log`、`kwifi.log`。卸载程序直接删 exe 和这个目录即可。

## 已知限制

- 控制热点必须管理员权限；拒绝 UAC 时程序仍驻留托盘，但无法开关热点
- 「开机自启热点」在登录时会弹一次 UAC（Windows 的硬性要求，无法静默提权）
- 隐身只处理 TTL / 组播 / IPv6 这类常见特征，不保证绕过所有检测手段
- 仅对自己拥有或被授权的网络使用

## 从源码构建

需要 [zig](https://ziglang.org/download/)（自带 C/C++ 编译器和链接器，无需 Visual Studio）：

```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
```

版本号取自仓库根目录的 [`VERSION`](VERSION)。产物是 `kwifi.exe`。

## Code signing policy

免费代码签名由 [SignPath.io](https://about.signpath.io) 提供，证书由
[SignPath Foundation](https://signpath.org) 签出 —— **申请中，尚未生效**。批准前的构建
未签名，Release 说明里附有 SHA-256，也可用 `tools\verify-signature.ps1` 核对。

- Committers and reviewers：仓库维护者
- Approvers：仓库所有者（每次签名请求需人工批准）
- 隐私：本程序不会向你未指定的联网系统传输任何信息。热点名、密码、设备名、限速都
  只保存在本地 `%LOCALAPPDATA%\Kwifi\data`，从不上传
- 所有二进制均由 [`.github/workflows/build.yml`](.github/workflows/build.yml) 从本仓库
  源码构建，没有仓库外产出的文件

## License

MIT，见 [LICENSE](LICENSE)。

第三方组件：数据面使用 [WinDivert](https://reqrypt.org/windivert.html)（LGPLv3 +
静态链接例外）；界面使用系统自带的 WebView2 运行时，不随程序分发。
