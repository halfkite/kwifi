# 代码签名：让所有人下载后都不再看到「未知发布者」

本文回答三件事：为什么自签名对别人无效、免费/付费有哪些路、以及这个项目接进 CI 的具体做法。

---

## 一、为什么 `tools\sign.ps1` 的自签名只能自己用

Windows 判断「发布者可信」的方式是**证书链能不能走到系统信任的根证书**：

```
你的签名证书 → 中间证书 → 根证书（已在 Windows「受信任的根证书颁发机构」里）
```

自签名证书的「链」就是它自己，Windows 默认不认。`sign.ps1` 把它塞进了**本机**的
受信任根，所以只在这台机器上有效。

想让所有人都不用装证书，根证书必须来自**微软信任根计划（Microsoft Trusted Root
Program）里的公共 CA**——DigiCert、Sectigo、Certum、SSL.com 这些。让 CA 给你签发，
就必须过身份审核（个人身份证 / 企业营业执照）。这一步没法技术绕过，也是「注册签名」
真正的含义：**不是注册，是买一张 CA 签发的证书**。

另外自 2023-06 起 CA/Browser Forum 规定代码签名私钥必须放在 **FIPS 140-2 Level 2
以上硬件**（UKey 或云 HSM），所以现在的证书都是「UKey + 证书」或「云签名账号」。

---

## 二、四条路对比

| 方案 | 费用 | 签发对象 | 适合本项目？ |
|---|---|---|---|
| **SignPath Foundation** | 免费 | SignPath Foundation（替开源项目背书） | ✅ 首选，但有条款风险，见下 |
| **个人 OV 证书**（Certum / Sectigo / SSL.com） | ¥600–900 / 年 | 你个人 | ✅ 稳，无需审核项目性质 |
| **组织 OV 证书** | ¥880–2000 / 年 | 你的公司/组织 | 有营业执照才可选 |
| **EV 证书** | ¥2300+ / 年 | 组织 | 贵，主要买 SmartScreen 声誉 |
| Azure Artifact Signing（原 Trusted Signing） | $9.99 / 月 | 组织 | ❌ 个人仅限美国/加拿大，组织需美/加/欧/英且满 3 年 |

> 价格随汇率和 CA 政策变动，以官网为准。参考：
> [Certum 三档报价](https://www.certumca.cn/news-list/55.html)、
> [Azure 签名资格说明](https://learn.microsoft.com/en-nz/answers/questions/6020935/artifact-signing)

---

## 三、路线 A：SignPath Foundation（免费，开源专用）

[SignPath Foundation](https://signpath.org/) 自己向 CA 申请证书（签发对象是
Foundation），再让通过审核的开源项目用它的 HSM 签名。签名后的文件发布者显示为
**SignPath Foundation**，同时能证明「这个二进制确实来自这个仓库的自动构建」。

### 申请步骤

1. 把仓库推到 GitHub 并**公开**，加 OSI 认可的开源许可证（本仓库已加 MIT：`LICENSE`）。
2. 至少发布一个 Release（有可下载的构建产物）。
3. 打开 <https://signpath.org/apply> → 填表：
   - 仓库地址
   - 项目简介（kwifi：Windows 热点管理工具）
   - 为什么需要签名（用户下载后被杀软/SmartScreen 拦、UAC 显示未知发布者）
4. 等审核，通常 **1–2 周**。只有仓库 owner / 有发布权限的维护者能代表项目申请。
5. 通过后在 <https://app.signpath.io> 拿到 organization id，创建 project / signing
   policy / artifact configuration，然后生成 API token 塞进 GitHub Secrets。

### 必须满足的条件（摘自官方条款）

- OSI 许可证，**不能有闭源组件**（商业双许可也不行）
- 项目活跃维护、已发布、功能有文档
- 所有维护者 **GitHub 与 SignPath 都开 2FA**
- 团队要有明确角色：Authors / Reviewers / **Approvers（每次签名需人工批准）**
- 项目主页要有 **Code signing policy** 段落（见文末模板）
- 二进制必须由 CI 从源码可验证地构建出来
- 签名产物的「产品名 / 产品版本」元数据要被固定并校验

### ⚠️ 这个项目容易被拒的地方

条款里有一条 **No hacking tools**：

> Software must not include features designed to identify or exploit security
> vulnerabilities or **circumvent security measures of their execution environment**.

kwifi 的「隐身模式」——TTL 归一、屏蔽组播探测、让下游设备不被校园网识别为共享
——在字面上非常接近「规避网络侧的检测/管控措施」。SignPath 的证书上写着他们的名字，
他们对这类功能通常很谨慎，**有较大可能被拒**。

几条现实建议：

1. **先照实申请**，但把隐身模式描述为"兼容性/隐私"功能，而不是"绕过检测"。
2. 如果被拒，可以做一个 **`kwifi-lite`**：去掉隐身与 TTL 改写，只保留热点 + 限速 +
   优先级 + 黑名单，用这个分支去申请。隐身作为可选模块单独发布（不带签名）。
3. 或者干脆走路线 B，自己买证书——CA 只审你的身份，不审软件用途。

---

## 四、路线 B：自己买一张 OV 证书（最确定的路）

### 申请

以 Certum 为例（国内可走环度网信等代理，也有中文站）：

| 档位 | 参考价 | 签发对象 | 说明 |
|---|---|---|---|
| 个人版 | ¥600 / 年 | 你的名字 | 个人开发者、开源项目够用 |
| 标准版（组织） | ¥880 / 年 | 公司名 | 需要营业执照 |
| EV | ¥2300 / 年 | 公司名 | 审核更严，SmartScreen 起点更好 |

流程：下单 → 提交身份证（个人版）或营业执照（组织版）+ 手机/邮箱核验 →
CA 电话核实 → 1–5 个工作日签发 → 拿到 **UKey** 或 **云签名账号**（SimplySign）。

国内代理参考：<https://www.certumca.cn/>

### 用 UKey 在本地签名

```powershell
# 装上 UKey 驱动 + 证书后，signtool 会自动找到它
signtool sign /a /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 .\kwifi.exe
signtool verify /pa /v .\kwifi.exe
```

`signtool.exe` 来自 Windows SDK（Visual Studio 生成工具里勾选 "Windows SDK Signing Tools"）。

### 在 GitHub Actions 里签名（云签名）

UKey 插不进 CI，所以要么用 CA 的云签名服务（Certum SimplySign、SSL.com eSigner），
要么用一个自建签名机。云签名一般提供 CLI + 账号密码/OAuth，把凭据放 Secrets，在
workflow 里调它的 CLI 签完再上传。

本项目已经写好 `.github/workflows/build.yml` 和 `release.yml`：

- 没有配置 Secrets 时：正常构建，产物未签名（开发预览）
- 配置了签名凭据时：自动签名后再发布

---

## 五、签完名还会看到 SmartScreen 吗

会，但**文字不同**：

| 状态 | UAC 里的发布者 | SmartScreen |
|---|---|---|
| 未签名 | 未知发布者 | 「Windows 已保护你的电脑」，要额外点「仍要运行」 |
| OV 签名 | 你的名字 / SignPath Foundation | 新文件仍可能提示，**下载量上来后自动消失** |
| EV 签名 | 你的组织名 | 通常直接通过 |

SmartScreen 的判定依据是**这个文件的下载/安装声誉**，不是签名的合法性。新版本、
新哈希等于重新从零攒声誉。所以：

- 每个 release 都保持同一个发布者名，声誉才会累积
- 别改文件名里的产品名
- 用固定 URL 的 release 页，别频繁换下载链接

---

## 六、验证签名（给用户看）

```powershell
Get-AuthenticodeSignature .\kwifi.exe | Format-List Status, SignerCertificate
```

或右键 exe → 属性 → 数字签名。

本仓库提供 `tools\verify-signature.ps1`，会把状态、签名者、时间戳一起打印出来。

---

## 七、Code signing policy（SignPath 要求放在项目主页）

> **Code signing policy**
>
> Free code signing provided by [SignPath.io](https://about.signpath.io),
> certificate by [SignPath Foundation](https://signpath.org).
>
> - Committers and reviewers: repository maintainers
> - Approvers: repository owners (each signing request is approved manually)
> - Privacy policy: this program does not transfer any information to other
>   networked systems unless specifically requested by the user or the person
>   installing or operating it.
>
> All binaries are built from this repository by the GitHub Actions workflow in
> `.github/workflows/build.yml`; no binary is produced outside CI.

（如果你走的是路线 B，把前两句换成你自己的证书信息，其余保留。）

---

## 八、一句话结论

- **想免费、又要所有人开箱即用** → 申请 SignPath Foundation，但先去掉/隔离「隐身」
  相关功能，否则大概率被拒
- **想省事、不想看别人脸色** → 买 Certum 个人版 OV（约 ¥600/年），签名信息是你自己的名字
- **只是自己和小圈子用** → 保持现状 + 在 README 写清「未签名，请核对 SHA256」，也完全够用
---

## 九、完全不用签名的两条路（2026-11 更新）

如果签名这条路暂时走不通，程序本身可以做到「打开不弹、只用一次 UAC」。这两条都
已经实现在程序里了。

### 9.1 启动不再请求提权（已实现）

以前双击 `kwifi.exe` 就会弹 UAC。现在改成**按需提权**：

| 操作 | 需要管理员 | 是否弹 UAC |
|---|---|---|
| 打开界面、看设备列表、看日志 | 否 | 不弹 |
| 改 SSID/密码、带宽策略、自启开关 | 否 | 不弹 |
| 改设备名、备注 | 否 | 不弹 |
| **开启热点 / 封禁 / 限速 / TTL / 隐身** | 是 | 点了才弹，一次 |

非管理员时界面顶部会有提示条，点「授予管理员权限」才弹 UAC。日常看设备完全不打扰。

### 9.2 免 UAC 启动任务（已实现，需手动开启）

Windows 的 UAC 只在**创建**高权限计划任务的瞬间问一次，之后该任务每次运行都
直接以管理员身份启动，**再也不弹**。程序内置了这个功能：

```powershell
kwifi.exe --install-task     # 装一次（会弹一次 UAC）
kwifi.exe --task-status      # 查询
kwifi.exe --remove-task      # 卸载
```

或直接在界面里操作（`/api/task`）。装上之后：

- 登录时自动以管理员身份启动，**没有任何弹窗**
- 连「未知发布者」都看不到，因为根本没有对话框
- 开机自启热点因此也能真正做到静默

代价：计划任务对普通用户不是完全透明的（能在任务计划程序里看到 `Kwifi\KwifiElevated`），
而且安装时需要一次管理员授权。卸载后一切恢复原状。

> 注意：这与你之前说的「别用计划任务」并不冲突——自启仍然走注册表 Run 键，
> 计划任务只用来**消除 UAC 弹窗**，两者是独立开关。

### 9.3 顺便：Certum 对中国开源项目有免费额度

[Certum 开源代码签名证书](http://certumcodesign.cn/shop/product/open-source-code-signing-cloud/)
的条款里写着：托管在 GitHub、**Star 数超过 1k** 的优秀开源项目可免费申请一个开源证书，
申请人需中国国籍。付费版 €49/年（约 ¥390）。

这条路需要项目先攒到 1k stars，但那之后就完全免费，且是真正的 CA 签名。

### 9.4 一句话总结

| 你的诉求 | 最短路径 |
|---|---|
| 只是不想每次打开都弹 UAC | 已实现，直接换新版 exe |
| 想开机静默启动、永不弹 | 装免 UAC 任务（`--install-task`） |
| 想让下载的人都不看到未知发布者 | 签名，见第二～四节 |
| 不想花钱 | 先攒 stars 申请 Certum 开源证书，或 SignPath |