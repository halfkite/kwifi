# 发布到 GitHub

仓库已经初始化并提交好了，版本定为 **0.6.0**。下面是最快的路径。

---

## 一键发布（推荐）

在能访问 github.com 的 PowerShell 里（代理已开）：

```powershell
cd D:\ai\redianwifi
powershell -ExecutionPolicy Bypass -File tools\publish.ps1
```

脚本会依次做：

1. 把 git 和 gh 指向代理 `127.0.0.1:7897`（可用 `-Proxy` 改）
2. 检查 gh 是否登录，没登录就带你走一遍 `gh auth login`
3. 创建公开仓库 `kwifi`，推送 `main`
4. 打 tag `v0.6.0` 并推送，触发 `release.yml` 自动构建发布

可重复执行：仓库/远程/tag 已存在时会跳过，不会重复创建。

参数：

```powershell
-Repo kwifi-hotspot      # 换仓库名
-Tag v0.6.0              # 换版本号
-Proxy http://127.0.0.1:7890   # 换代理端口
-SkipTag                 # 只推代码，不发布
```

---

## 手动做

### 第 1 步：登录（只做一次）

```powershell
$env:HTTPS_PROXY = "http://127.0.0.1:7897"
gh auth login
```

选：`GitHub.com` → `HTTPS` → `Yes` → `Login with a web browser`
记住 8 位码 → 回车开浏览器 → 粘贴 → 授权。

### 第 2 步：建仓库并推送

```powershell
git config --global http.https://github.com.proxy http://127.0.0.1:7897
cd D:\ai\redianwifi
gh repo create kwifi --public --source=. --remote=origin --push
```

### 第 3 步：发 0.6.0

```powershell
git tag v0.6.0
git push origin v0.6.0
```

2～4 分钟后 `https://github.com/<你>/kwifi/releases` 就有 `kwifi.exe` 和 `SHA256SUMS.txt`。

---

## 发布前已做的清理

| 项 | 状态 |
|---|---|
| 账号密码（`data/config.json`） | 未提交，在 `.gitignore` 里 |
| 设备历史（`data/devices.json`） | 未提交 |
| 编译产物（`kwifi.exe`、`embedded.h`、`.o`） | 未提交，CI 现场生成 |
| `legacy/`（旧 Node 后端） | 未提交 |
| `docs/re-connectify.md`（对商业软件逆向的记录） | **未提交**，见下方说明 |

> **关于 `docs/re-connectify.md`**：这份文件是对 Connectify 商业软件的逆向分析记录。
> 公开发布有法律风险，也可能导致 SignPath 拒绝签名申请。它留在你的本地磁盘上，
> 只是不在 git 里。

---

## 版本号在哪

| 位置 | 值 |
|---|---|
| git tag | `v0.6.0` |
| exe 文件属性 | `0.6.0.0`（CI 从 tag 推导） |
| `native/build.ps1` 默认值 | `0.6.0.0` |

改版本只需打新 tag：`git tag v0.6.1 && git push origin v0.6.1`。

---

## 常见问题

**`dial tcp 57.155.223.240:443` 超时**
DNS 被污染，gh 不读 Windows 系统代理。先 `$env:HTTPS_PROXY="http://127.0.0.1:7897"`。

**`HTTP 401: Bad credentials`**
Token 不对。classic token 固定 40 位（`ghp_` + 36 位），复制时容易少一位。用浏览器登录更稳。

**`Permission denied (publickey)`**
走了 SSH 但没密钥。改 HTTPS：`git remote set-url origin https://github.com/<你>/kwifi.git`

**CI 构建失败**
看 Actions 日志。本地先验证：`powershell -ExecutionPolicy Bypass -File native\build.ps1 -Version 0.6.0.0`

**想让 CI 里也能签名**
先按 [SIGNING.md](SIGNING.md) 申请，然后在
`Settings → Secrets and variables → Actions` 里加配置，见 `.github/workflows/release.yml` 顶部注释。

---

## 后续常用命令

```powershell
git status                                    # 看改动
git add -A; git commit -m "..."               # 提交
git push                                      # 推送
git tag v0.6.1; git push origin v0.6.1        # 发新版
gh run watch                                  # 看 CI
gh repo view --web                            # 浏览器打开仓库
```
