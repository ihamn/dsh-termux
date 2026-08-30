# DeepSeek Harness — Termux 社区适配版

> ⚠️ **本仓库是社区适配分支，不是 DeepSeek 官方项目。**
>
> 本分支基于 [deepseek-ai/deepseek-harness](https://github.com/deepseek-ai/deepseek-harness)
> （MIT 开源协议）的 `0.1.2-alpha.1`，面向 **Termux (Android)** 做了移动端网页界面
> 适配与 Android 兼容性修复，**与 DeepSeek 官方无任何关联，未获其授权或背书**；
> "DeepSeek"、"DeepSeek Harness" 等名称与商标归其各自所有者所有。

---

## 这是什么

DeepSeek Harness（`dsh`）官方开源 Agent Harness 的 **Termux 社区适配版**，包含：

- 📱 **移动端网页界面适配**：抽屉式侧栏/详情面板、全宽聊天、统计行换行等；
- 🔧 **Android 兼容性修复**：f2fs 硬链接（link→rename 回退）、subprocess 平台识别、
  bionic 原生模块编译、pnpm 9 安装链路、ripgrep 桥接；
- 🚀 **一键环境配置**：`bash docs/termux/setup-termux.sh`。

**📖 安装与使用指南**：[docs/termux/README.md](docs/termux/README.md)

## 快速开始（Termux）

```sh
pkg install -y git
git clone https://gitee.com/ihamn/dsh-termux.git
cd dsh-termux
bash docs/termux/setup-termux.sh
```

启动并打开网页版：

```sh
bash docs/termux/start-dsh-web.sh --bg
# 手机浏览器打开 http://127.0.0.1:3080
```

> 没有 git 也可以在仓库页下载 ZIP 解压后执行同一脚本；详细说明见
> [docs/termux/README.md](docs/termux/README.md)。

## 本分支改动

完整改动清单见 [docs/termux/README.md §6](docs/termux/README.md#6-本分支包含的适配改动)。

## 许可

[MIT](LICENSE)，保留上游 DeepSeek 版权声明，并追加本分支的版权行。
