# SignPath Foundation 申请与接入

免费给开源项目做代码签名，签名后发布者显示为 **SignPath Foundation**，用户不再看到
「未知发布者」，SmartScreen 也会逐步放行。

官方：<https://signpath.org> · 条款：<https://signpath.org/terms>

---

## 一、申请前自查

| 条件 | 我们的状态 |
|---|---|
| 公开的 GitHub 仓库 | ✅ <https://github.com/halfkite/kwifi> |
| OSI 认可的开源许可证 | ✅ MIT |
| 至少发布过一个 Release | ✅ v0.6.0 |
| 由 CI 从源码构建（不能手工上传） | ✅ `.github/workflows/build.yml` |
| 主页有 Code signing policy 段落 | ✅ README 末尾 |
| 维护者 GitHub 开 2FA | ⚠️ **你需要确认** → <https://github.com/settings/security> |
| 团队角色（Authors / Reviewers / Approvers） | ⚠️ 单人项目也要写清楚 |
| 产品名/版本在二进制里固定且可校验 | ✅ `ProductName=kwifi`、`FileVersion=0.6.0.0` |

### ⚠️ 两个真实风险

**1. 项目太新，没有可验证的声誉**

条款原文：
> we cannot sign binaries based on source code that nobody knows. For executable
> programs that may be downloaded and executed based on our signature, we require
> a certain verifiable reputation.

仓库刚建、0 star、0 下载量，可能被以"缺乏可验证声誉"为由拒绝。**建议先攒一点**
（发布几个版本、写进相关社区、有下载记录）再申请，或者现在就试一次——被拒也不亏。

**2. 隐身模式可能撞上「No hacking tools」条款**

条款禁止「规避执行环境安全措施」的功能。TTL 归一、屏蔽组播探测、让下游设备不被
识别为共享，字面上接近这一类。

**应对**：申请时把隐身模式描述为**隐私与兼容性**功能（减少设备在共享网络上的不必要
暴露），**不要**写成"绕过校园网检测"。如果被拒，可以做 `kwifi-lite`（去掉隐身和 TTL
改写）单独申请。

---

## 二、提交申请

打开 <https://signpath.org/apply>，填表。下面是可以直接用的英文答案：

**Project name**
```
kwifi
```

**Repository URL**
```
https://github.com/halfkite/kwifi
```

**License**
```
MIT
```

**Project description**
```
kwifi is a Windows 10/11 hotspot console. It turns a Wi-Fi adapter into an access
point, lists connected devices, and lets the user cap each device's upload and
download speed, assign priority tiers, and keep a blocklist. Everything runs
locally; no data leaves the machine.

It ships as a single ~1 MB executable written in C, with no runtime dependency
(other than the WebView2 runtime that ships with Windows). The previous
implementation was Node.js-based and required bundling a 90 MB runtime, which is
why it was rewritten as a native binary.

The user interface is a plain HTML/CSS/JS page embedded in the executable and
rendered by WebView2.
```

**Why do you need code signing?**
```
The program needs administrator rights to configure the hotspot, so Windows shows
a UAC prompt on first use. Because the binary is unsigned, that prompt says
"unknown publisher", which makes users distrust the download and is a common
source of "is this malware?" questions.

Signing would:
- show a verified publisher in the UAC dialog and in Explorer
- let users verify the download came from this repository's CI build
- reduce SmartScreen friction for a tool that has to be run as administrator
```

**Anything else we should know?**（可选）
```
All binaries are produced by GitHub Actions from this repository; there are no
out-of-band builds. The build is reproducible from a clean checkout and needs
only zig (the C compiler is vendored through zig, the WebView2 headers are in
`native/wv2shim/`). Version metadata is enforced at build time
(ProductName=kwifi, FileVersion=<tag>), and the release workflow publishes exactly
one file: kwifi-<version>.exe.
```

---

## 三、批准后的接入步骤

### 1. 在 SignPath 后台建三样东西

登录 <https://app.signpath.io>，进入你的 organization：

| 项目 | 值 | 说明 |
|---|---|---|
| Project slug | `kwifi` | 建 Project 时填 |
| Signing policy slug | `release-signing` | 需要人工批准的策略 |
| Artifact configuration slug | `exe` | 见下面的 XML |

### 2. Artifact configuration

在 Project → Artifact Configurations 新建，slug 填 `exe`，内容是：

```xml
<artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
  <pe-file path="kwifi-*.exe">
    <authenticode-sign />
  </pe-file>
</artifact-configuration>
```

如果后台要求固定文件名，就把 `kwifi-*.exe` 换成具体名字；我们的 CI 上传的产物名是
`kwifi-<version>.exe`。

> SignPath 条款要求**固定产品名与版本**。我们的 exe 已经带
> `ProductName=kwifi` 和 `FileVersion=<tag>`，这是用 `native/respatch.c` 写进去的。
> 如果要显式声明限制，可以加 `<metadata-restrictions>`，但当前设置已满足要求。

### 3. 拿 API token

SignPath 后台 → User settings → API tokens → 新建，权限选
**"Submitter"**。

### 4. 填进 GitHub

仓库 → `Settings` → `Secrets and variables` → `Actions`：

| 类型 | 名称 | 值 |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | 上一步的 token |
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath 后台 URL 里那串 GUID |
| Variable | `SIGNPATH_PROJECT_SLUG` | `kwifi` |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | `release-signing` |
| Variable | `SIGNPATH_ARTIFACT_CONFIGURATION_SLUG` | `exe` |

### 5. 发版

```powershell
# 改 VERSION 后打新 tag
"0.6.1" | Set-Content VERSION -NoNewline
git add VERSION; git commit -m "bump 0.6.1"
git tag v0.6.1
git push origin main --tags
```

工作流会：构建 → 上传未签名产物 → 提交签名请求 → **等你在 SignPath 后台批准** →
下载已签名产物 → 发布。Release 说明里的「签名」一栏会变成 `Valid`。

---

## 四、批准可能很慢

SignPath 的 OSS 申请通常 **1–2 周**，而且每次发版都要人工批准一次签名请求（这是条款
要求，不是配置问题）。如果你发版频繁，这会比较麻烦——可以只在重要版本上签名。

---

## 五、如果被拒

1. **先用付费证书顶上**：Certum 个人 OV 约 ¥600/年，只审身份不审用途，
   详见 [SIGNING.md](SIGNING.md)
2. **改造后重投**：做 `kwifi-lite`（去掉隐身和 TTL 改写），删掉相关描述后重新申请
3. **先攒声誉再申请**：多几个版本、有下载量、被别的项目引用之后再投

被拒不影响现在的一切——未签名的版本照常发布，Release 说明里有 SHA-256 可核对。
