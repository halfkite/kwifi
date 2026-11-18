# 发布到 GitHub

仓库已经初始化好并提交了第一个 commit，剩下的是**创建远程仓库并推送**。下面三步
大约 3 分钟。

---

## 第 1 步：登录 GitHub（只做一次）

打开 PowerShell，运行：

```powershell
gh auth login
```

按提示选：

```
? What account do you want to log into?            GitHub.com
? What is your preferred protocol for Git operations?  HTTPS
? Authenticate Git with your GitHub credentials?       Yes
? How would you like to authenticate?                  Login with a web browser
```

它会显示一个 **8 位验证码**（形如 `A1B2-C3D4`），按回车打开浏览器，粘贴验证码，
授权即可。

> 如果你已经登录过 GitHub（`git push` 以前成功过），可以跳过这一步。

验证：

```powershell
gh auth status
```

---

## 第 2 步：创建仓库并推送

在仓库目录里执行：

```powershell
cd D:\ai\redianwifi
gh repo create kwifi --public --source=. --remote=origin --push
```

这一条命令做四件事：在 GitHub 上建一个叫 `kwifi` 的公开仓库、把 `origin` 指过去、
推送 `main`、设置上游分支。

如果仓库名被占用，换个名字：

```powershell
gh repo create kwifi-hotspot --public --source=. --remote=origin --push
```

### 不想用 gh 命令

在 <https://github.com/new> 手动建一个**空**仓库（不要勾 README / .gitignore / license），
然后：

```powershell
cd D:\ai\redianwifi
git remote add origin https://github.com/<你的用户名>/kwifi.git
git push -u origin main
```

---

## 第 3 步：发第一个 Release

打 tag 推上去，`release.yml` 会自动构建并发布：

```powershell
git tag v1.0.0
git push origin v1.0.0
```

2～4 分钟后到 `https://github.com/<你的用户名>/kwifi/releases` 就能看到：

- `kwifi.exe`
- `README.md`、`LICENSE`
- `SHA256SUMS.txt`

如果构建失败，点仓库的 **Actions** 标签页看日志。

### 只想先跑一次构建、不发版

```powershell
gh workflow run build.yml
gh run watch
```

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

当前仓库 36 个文件、约 1 MB。

---

## 后续常用命令

```powershell
git status                    # 看改动
git add -A && git commit -m "..."   # 提交
git push                      # 推送
git tag v1.0.1 && git push origin v1.0.1   # 发新版
gh repo view --web            # 浏览器打开仓库
```

---

## 常见问题

**`gh: command not found`**
GitHub CLI 没装：<https://cli.github.com/> 或 `winget install GitHub.cli`。

**推送提示 `Permission denied (publickey)`**
说明走的是 SSH 但没配密钥。改用 HTTPS：

```powershell
git remote set-url origin https://github.com/<你的用户名>/kwifi.git
```

**`raw.githubusercontent.com` 上的 README 图片不显示**
`docs/*.png` 已提交，路径是相对路径，正常会显示。

**想让 CI 里也能签名**
先按 [SIGNING.md](SIGNING.md) 申请，然后在仓库
`Settings → Secrets and variables → Actions` 里加 5 项配置，见 `.github/workflows/release.yml`
文件顶部的注释。
