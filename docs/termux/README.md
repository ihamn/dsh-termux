# DeepSeek Harness 0.2.0-rc.1 — Termux (Android ARM64) 适配说明

本文档记录本分支在 Termux（Android / aarch64）上从源码构建、运行和日常使用的完整方法。
本分支基于 DeepSeek Harness `0.2.0-rc.1`（官方 master，2026-09-28 同步），包含移动端
网页界面适配与 Android 兼容性修复，
所有改动均可在 `git diff` 中逐文件审阅。

> **免责声明**：本项目是社区爱好者基于 DeepSeek Harness（MIT 开源协议）的
> Termux 适配分支，**与 DeepSeek 官方无任何关联，也未获其授权或背书**。
> "DeepSeek"、"DeepSeek Harness" 等名称与商标归其各自所有者所有。

> 目标读者：想在 Android 手机上跑 DeepSeek Harness 网页版的开发者。

---

## 1. 环境要求

| 项目 | 要求 |
| --- | --- |
| 设备 | Android（arm64 / aarch64），建议 8 核 + 4GB 内存以上 |
| 运行环境 | Termux（F-Droid 版） |
| Node.js | ≥ 22（本文实测 v26.4.0，`pkg install nodejs-lts`） |
| 存储 | 构建后约占用 2GB（依赖 + 缓存），建议预留 3GB |

### 1.1 本适配的参考环境（实测基线）

以下为开发本分支时实际构建、运行并验证过的环境。其他版本理论上兼容，
但未逐一验证；遇到问题时请先对照本节差异排查。

| 项目 | 实测基线 |
| --- | --- |
| 设备 | aarch64 / 8 核 Android 手机（型号编码 22011211C） |
| Android | 14（SDK 34），内核 5.10.209-android12 |
| Termux | v0.119.0-beta.3（GitHub 发布版，Android 7+ 用 `apt-android-7` 变体），软件源 mirrors.pku.edu.cn |
| Node.js | v26.4.0（`nodejs-lts` 包） |
| npm | 11.19.0 |
| pnpm | 9.15.9（项目必需，见 §2.2） |
| clang | 21.1.8 |
| cmake | 4.4.2 |
| binutils | 2.47 |
| ndk-sysroot | 29 |
| python | 3.14.6 |
| ripgrep | 15.2.0 |
| npm 镜像 | registry.npmmirror.com |
| 适配时间 | 2026-08-13 ~ 08-14（初版 rc.5）；2026-08-30（0.1.2-alpha.1）；2026-09-28（0.2.0-rc.1） |

安装基础工具链：

```bash
pkg install nodejs-lts cmake make clang binutils ndk-sysroot python ripgrep git
```

> `ndk-sysroot` 提供 bionic 头文件，是原生模块编译的必要依赖。

---

## 2. 获取源码与安装依赖

### 2.0 快速开始（一条命令）

不想手动一步步来？直接运行环境自动配置脚本，它会完成 §2-§4 的全部内容
（工具链、pnpm 9、依赖、原生模块编译/桥接、构建）：

```bash
bash docs/termux/setup-termux.sh
```

脚本会先展示完整声明并请求确认，其中会**单独询问**是否授予手机共享存储访问
权限（`termux-setup-storage`，将弹出系统授权窗口）；跳过也不影响安装，
之后随时可手动运行 `termux-setup-storage` 补授权。
构建完成后脚本还会**可选询问**是否配置 DeepSeek API Key（静默输入，
写入 `~/.bashrc`）；跳过的话，可稍后在网页端 Settings → Models 配置
第三方提供商，或在启动前手动 `export DEEPSEEK_API_KEY=sk-xxx`。

> **验证说明**：脚本的每个步骤（工具链、pnpm 9、依赖安装、koffi / node-pty
> 编译、rg 桥接、三阶段构建、启动运行）均已在 §1.1 参考环境上**分步实测通过**；
> 一键整跑建议在干净环境试跑一次，如遇问题请先对照 §1.1 核对环境差异。

### 2.1 克隆本仓库

```bash
git clone <本仓库地址> deepseek-harness
cd deepseek-harness
```

### 2.2 安装 pnpm 9（关键）

**必须使用 pnpm 9.x（本分支实测 9.15.9），不要用 pnpm 10/11。** 原因如下：

pnpm 10/11 自带原生二进制（`@pnpm/exe`），运行时强制校验其
`@pnpm/exe.android-arm64` 原生二进制是否登记在 `pnpm-lock.yaml` 中。官方锁文件
通常在 x86_64 上生成，只登记 `linux-x64` 等条目，因此 Android ARM64 上
pnpm 10/11 会拒绝执行 `install` / `run` 等全部命令：

```text
[ERROR] Cannot verify the identity of the @pnpm/exe.android-arm64 native binary:
it is missing from pnpm-lock.yaml.
```

该校验无法通过配置绕过（`--ignore-scripts`、`--no-frozen-lockfile`、环境变量、
手工编辑锁文件等均已实测无效）。**pnpm 9 直接以 Node 脚本运行，没有这道校验**，
且原生支持官方锁文件的 v9.0 格式，无需改写锁文件。

安装（国内网络下 registry.npmjs.org 实测可直连；npmmirror 的 cdn 偶发超时，
可换 `https://mirrors.huaweicloud.com/repository/npm/`）：

```bash
npm install -g pnpm@9.15.9 --registry=https://registry.npmjs.org
pnpm --version   # 9.15.9
```

### 2.3 安装依赖

```bash
pnpm install
```

pnpm 9 直接读取官方 v9.0 锁文件，**不需要改写**（见 §7）。
安装完成后 `node_modules` 中存在若干"待构建"（pending）的原生模块，属正常现象，
后续按 §3 处理。

---

## 3. 原生模块编译（Termux 特供）

以下模块没有 android-arm64 预编译二进制，必须在本机源码编译或桥接。

### 3.1 koffi（FFI 库，编译补丁必需）

koffi 的源码在 `node_modules/.pnpm/koffi@3.1.1/node_modules/koffi`。
其 C++ 代码在 Android（bionic）下有一处编译错误：`statx()` 的声明与 glibc 不同。
应用本仓库提供的补丁（`docs/termux/koffi-android.patch`）后编译：

```bash
cd node_modules/.pnpm/koffi@3.1.1/node_modules/koffi
patch -p1 < <仓库根>/docs/termux/koffi-android.patch
node ./cnoke.cjs -P . -D src/koffi --prebuild --release
```

编译产物为 `build/koffi/android_arm64/.../Output/koffi.node`，可被运行时自动发现。

### 3.2 node-pty（PTY 子进程）

node-gyp 默认会从 nodejs.org 下载头文件，国内网络常被掐断导致卡死。
Termux 已自带 Node 头文件（`$PREFIX/include/node`），直接用本地头文件编译：

```bash
cd node_modules/.pnpm/node-pty@1.2.0-beta.15/node_modules/node-pty
node $(npm root -g)/npm/node_modules/node-gyp/bin/node-gyp.js rebuild --nodedir=$PREFIX
```

> pnpm 9 不从 `pnpm-workspace.yaml` 读取 `patchedDependencies`，因此官方给 node-pty 的
> spawn-helper 路径补丁在 Termux 上**不会自动应用**——实测也不需要：安卓构建只产出
> `pty.node`，node-pty 走 forkpty 路径，没装 `spawn-helper` 也能正常开 PTY
> （本分支已实测 spawn 通）。

### 3.3 ripgrep 桥接（glob/grep 工具）

harness 的 `glob` / `grep` 工具通过 `@vscode/ripgrep` 包定位 rg 二进制，
而该 npm 包没有 android 预编译。安装系统 rg 后桥接：

```bash
pkg install ripgrep
mkdir -p node_modules/@vscode/ripgrep
```

直接把仓库里 `docs/termux/ripgrep-bridge/` 下的两个文件复制过去：

```bash
mkdir -p node_modules/@vscode/ripgrep
cp docs/termux/ripgrep-bridge/package.json docs/termux/ripgrep-bridge/index.js node_modules/@vscode/ripgrep/
```

### 3.4 sharp（图片处理，WebAssembly 模式）

sharp 没有 android 运行时，需使用 WebAssembly 版本。官方锁文件已包含
`@img/sharp-wasm32@0.35.3`（sharp 的可选依赖），pnpm 安装后通常已就位；
若缺失再手动安装：

```bash
mkdir -p /tmp/sharpw && cd /tmp/sharpw
npm install @img/sharp-wasm32@0.35.3 --registry=https://registry.npmmirror.com
cp -r node_modules/@img/sharp-wasm32 <仓库>/node_modules/@img/
cp -r node_modules/@emnapi node_modules/tslib <仓库>/node_modules/   # 若缺
```

### 3.5 模块加载器（--expose-internals 回退）

0.2 起 harness 通过原生插件 `node-addon-require-builtin` 取 Node 内部模块解析器，
而该包没有 android-arm64 预编译。本分支在
`packages/boot/app-boot/src/profile-resolution/resolver.ts` 里加了回退：**加载不到原生
插件时改走 `require('internal/...')`**，由 Node 自带的 `--expose-internals` 提供同样能力。

因此**运行时必须携带 `--expose-internals`**（见 §5）；缺了它启动会报
`No usable native binding found for node-addon-require-builtin-android-arm64`。

### 3.6 安卓原生沙箱 runner（read-only / workspace-write）

harness 自带的两个 Linux 后端在 Android 上都不可用：`bwrap` 需要非特权 user
namespace（本机 `unshare --user` 直接 `Invalid argument`），Landlock 需要
Linux ≥ 5.13 且内核编译时开启（本机为 5.10）。因此上游组合里受限模式会
fail-closed 报 `SANDBOX_UNAVAILABLE`。

本分支补了一个约 16KB 的原生 runner（无 VM / 无容器 / 无 root / 无常驻进程）：
用 seccomp `SECCOMP_RET_USER_NOTIF` 只拦写入类系统调用，监督进程按路径判定
`allow` / `EACCES`。安装：

```bash
bash docs/termux/sandbox/install.sh          # 编译并接线到 web profile
```

设计与限制见 [sandbox/README.md](sandbox/README.md)（合作式边界、无 PID/网络
隔离、`/tmp` 映射到 `$TMPDIR` 等）。`setup-termux.sh` 已包含这一步。

---

## 4. 构建

```bash
pnpm run build:lib:host      # 服务端（tsc + tsdown，约 5-8 分钟）
pnpm run build:lib:client    # 客户端插件包（约 3-5 分钟）
pnpm --filter @deepseek-ai/dsh-web-frontend run build   # 网页前端（约 25 秒）
```

构建产物在各自包的 `lib/` 下（已被 .gitignore 排除，不入库）。

> **安卓注意（`build:lib:host` 的最后一步）**：该脚本末尾会调用
> `pnpm --filter @deepseek-ai/dsh-desktop run bundle` 打包 Electron 桌面端。脚本里的
> `pnpm` 会解析到 `node_modules/.bin/pnpm`（即 `packageManager` 钉住的 11.x），触发依赖
> 校验并试图清空 `node_modules`，无 TTY 时报
> `ERR_PNPM_ABORTED_REMOVE_MODULES_DIR_NO_TTY`。**安卓不需要桌面端**，把前两步单独跑即可：
>
> ```bash
> node --max-old-space-size=4096 ./node_modules/typescript/bin/tsc -b tsconfig.host.json
> ./node_modules/.bin/tsdown --env.DSH_BUILD_FACE host
> ```
>
> 另外：**升级前务必删掉旧构建残留**（`lib/`、`*.tsbuildinfo`，以及已从上游删除、只剩
> `node_modules` 的空包目录）。否则 tsdown 会把这些空目录当工作区成员，`readPackageJson`
> 向上继承到根包，报 `[@deepseek-ai/dsh-root] Cannot find entry`。

---

## 5. 运行

```bash
export DEEPSEEK_API_KEY=sk-xxx
node --expose-internals apps/cli/lib/bin.js --profile web
```

装上 §3.6 的沙箱 runner 后，默认档位是 `workspace-write` + 审批：只有会话工作区
与 `$TMPDIR` 可写，越界写入会被拒绝，模型可以就同一条命令申请一次更宽权限，
网页端弹审批（与电脑端一致）。没装 runner 时 `start-dsh-web.sh` 会退回
`danger-full-access` 并给出提示。想强制某档：

```bash
DSH_PERMISSION_MODE=read-only bash docs/termux/start-dsh-web.sh
```

启动成功后输出：

```text
dsh web: http://127.0.0.1:3080
```

> **本分支已对 loopback 访问免 token**（官方新版默认启用浏览器会话 token 鉴权，
> 本分支在 `packages/client/connection/src/browser-auth.ts` 中放行了 loopback
> 来源——服务只监听 127.0.0.1，`/api` 的 Host/Origin 栅栏仍会拒绝非 loopback
> 请求，信任边界与旧版一致）。手机浏览器直接打开 `http://127.0.0.1:3080/` 即可。
> 若后续通过 `--trusted-host` 开放局域网访问，非 loopback 来源仍需 token。

### 5.1 三种权限模式与安卓原生沙箱

没有 runner 时，受限模式会 fail-closed 报错：

```text
sandbox mode "workspace-write" is requested but no sandbox backend is usable on this host
```

装上 §3.6 的 runner 之后，网页端权限选择器里的三档都可用，行为与电脑端一致：

| 档位 | 效果 |
| --- | --- |
| `read-only` | 除设备节点与 `/proc/self/*` 外全部只读；写、`chmod`、`rename`、`ptrace`、`mount` 等一律 `EACCES` |
| `workspace-write` | 额外允许会话工作区与 `$TMPDIR` 写入（默认档） |
| `danger-full-access` | 不经过 runner 的原生放行，审批策略为 `never`；需要完全放行时用这一档 |

被拒绝的命令会带 `[sandbox: file access denied under <mode> mode]`，模型可在同一轮次
申请一次更宽模式并附理由，网页端弹审批——这就是电脑端那套「拒绝→升权」的流程。
`danger-full-access` 下 agent 仍可读写 Termux 权限范围内的文件，仅在需要时启动、
用完关闭依旧是最直接的风控手段。

边界说明：沙箱约束的是 Termux 进程族的文件效果。通过 IPC/Binder 委托给其它应用或
系统服务执行的操作发生在沙箱之外，不在其管辖范围内（那类通道有自己的授权机制）；
需要这类用法时请切到 `danger-full-access`。细节见
[sandbox/README.md](sandbox/README.md)。

### 5.2 配置 API Key 的三种方式

1. **安装脚本内配置**（最省事）：`setup-termux.sh` 最后会询问并写入 `~/.bashrc`；
2. **手动导出**：启动前执行 `export DEEPSEEK_API_KEY=sk-xxx`
   （可写进 `~/.bashrc` 永久生效）；
3. **网页端配置**：Settings → Models 可添加/编辑提供商与 API Key，
   适合第三方/自定义提供商（DeepSeek 官方默认仍走 `DEEPSEEK_API_KEY` 环境变量）。

### 5.3 一键启动脚本

仓库提供 `docs/termux/start-dsh-web.sh`（Termux 版）：

```bash
pwd; ls -la                                  # 先确认当前目录与脚本都在（家目录下应有 start-dsh-web.sh 副本）
bash start-dsh-web.sh --bg                   # 后台运行 + wake-lock（息屏不冻），日志 ~/dsh-web.log
bash docs/termux/start-dsh-web.sh            # 或在仓库里跑这个，前台运行
```

> 启动脚本会把工作目录切到仓库根，`process.cwd()` 就是新会话的默认工作区
> （也是 `workspace-write` 允许写入的根）。想让别的目录成为工作区，就在那个目录下
> 直接跑 `node --expose-internals <仓库>/apps/cli/lib/bin.js --profile web`。

> 新版 CLI 已拒绝 `--host 0.0.0.0`（安全原因），如需局域网访问请改用
> `--trusted-host <手机IP>` 并在系统防火墙放行端口（token 仍会生效）。

停止后台服务：

```bash
pkill -f '^node --expose-internals apps/cli/lib/bin.js'
```

### 5.4 Zen 免费模型与 DeepSeek 官方 API 一键切换（可选）

仓库提供 `docs/termux/switch-llm.sh`，可在「OpenCode Zen · DeepSeek V4 Flash Free
（免费）」与「DeepSeek 官方 API」之间热切换，**无需重启服务**（harness 的
`settings.yaml` / `.credentials.yaml` 支持热重载，约 1 秒生效）：

```bash
bash docs/termux/switch-llm.sh zen         # 切到 Zen 免费模型（新会话默认生效）
bash docs/termux/switch-llm.sh official    # 切回 DeepSeek 官方 API
bash docs/termux/switch-llm.sh status      # 查看当前配置
```

首次切 `zen` 会提示输入 OpenCode Zen API Key（输入不回显）。两个密钥都只写入
`~/.dsh/.credentials.yaml`（权限 0600），脚本与仓库内**不含任何密钥**。

已知要点：

- Zen 免费模型 ID 为 `deepseek-v4-flash-free`，端点 `https://opencode.ai/zen/v1`
  （OpenAI 兼容，harness 会自动拼 `/chat/completions`）；
- 免费期输入/输出/缓存全免费，但**请求数据可能被 OpenCode 用于改进模型**，
  请勿发送敏感/机密内容；
- Zen 免费模型输出上限 131072 tokens，脚本已把 `maxTokens` 固定为 131072
  （harness 默认 256000 会被 Zen 以 400 拒绝）；
- 免费档有滚动窗口限流（常见为 429 Rate limit exceeded），撞上后重试 2 次仍失败，
  可等窗口重置或 `switch-llm.sh official` 临时切回官方。

> 切换只影响**新会话**的默认模型；已有会话保留原模型，可在会话内模型选择器手动切换。

### 5.5 配置第三方 LLM 提供商（可选）

除 DeepSeek 官方外，还可以通过 `llm-pi-ai` 接入 OpenAI 兼容提供商。
编辑 `~/.dsh/settings.yaml`：

```yaml
llm-pi-ai:
  providers:
    openai:
      apiKeyEnv: OPENAI_API_KEY
    openrouter:
      apiKeyEnv: OPENROUTER_API_KEY
      baseURL: https://openrouter.ai/api/v1
```

并在仓库根目录 `.env` 中设置对应密钥：

```sh
echo "OPENAI_API_KEY=sk-xxxx" >> .env
echo "OPENROUTER_API_KEY=sk-xxxx" >> .env
```

### 5.6 插件管理（可选）

harness 的插件装在 web profile 里（`~/.dsh/profiles/web`），装完**重启 dsh web** 生效：

```bash
# 可视化插件市场（设置 → 插件 → 插件市场，浏览/搜索/一键安装社区插件）
node --expose-internals apps/cli/lib/bin.js plugin --profile web add -w dshmarket

# 实用插件示例
node --expose-internals apps/cli/lib/bin.js plugin --profile web add -w dsh-chatvoice      # 语音输入 + 回复朗读（免费，浏览器原生）
node --expose-internals apps/cli/lib/bin.js plugin --profile web add -w dsh-mermaid-render # 对话内 Mermaid 图表渲染（离线引擎）
```

### 5.7 让 AI 自己管理插件（0.2 原生）

0.2 官方已内置 `plugin_manager` 工具（`packages/boot/plugin-manager/src/tools.ts`），
agent 可以直接**列出 / 启停 / 安装 / 卸载**插件与 bundle，无需手动敲 CLI。直接说：

```text
看看现在装了哪些插件
帮我装 dsh-chatvoice
```

该工具每次调用都需要 danger-full-access 或逐次审批；变更按 profile 生效，装完重启 dsh web。

> 0.2 会校验插件的 peerDependencies。本机原有两个旧插件（`dshmarket@1.38.0`、
> `@dhicoc/dsh-reverse-skill@1.0.5`）因版本区间不匹配 0.2 被**自动跳过**（日志会提示），
> 需要时用 `dsh plugin allow-version` 显式豁免或升级插件。

---

## 6. 本分支包含的适配改动

所有改动集中、可审阅，`git diff` 即可逐项核对：

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `packages/session/session-persistence-jsonl/src/index.ts` | 原子发布 `link()` 失败且为 EACCES/EPERM 时回退 `rename()` | Android f2fs 拒绝硬链接；保留官方 EEXIST 去重语义 |
| `packages/attachment/attachment-local/src/store.ts` | 同上；另在目录 fsync 打不开句柄时跳过 | 同上；且 `/`、`/data`、`/data/data` 对非特权应用不可读 |
| `packages/subprocess/subprocess-local/src/process-inspector.ts` | `android` 视同 `linux` | Android 无独立进程表实现，`process.platform === 'android'` 会导致终端检测报错 |
| `packages/boot/app-boot/src/profile-resolution/resolver.ts` | 原生插件 `node-addon-require-builtin` 加载失败时回退 `require('internal/...')` | 该插件无 android-arm64 预编译；回退由 `--expose-internals` 提供同样能力（见 §3.5） |
| `packages/client/ui-layout/src/client/AppFrame.tsx` / `.module.css` | 手机端抽屉式侧栏/详情面板、悬浮菜单按钮、显式 grid-column | 桌面三栏布局在窄屏“挤”，按官网模式改抽屉。0.2 右栏类名为 `.rightbarCol` |
| `packages/client/ui-conversation/.../skeleton/ConversationMainPanel.tsx` / `ConversationRoot.module.css` | 会话标题在手机端避让菜单按钮 | 标题被悬浮按钮遮挡。0.2 把骨架拆到了 `ConversationMainPanel` |
| `packages/client/ui-conversation/.../input/editor/keymap.ts` | 触屏设备上普通回车=换行，Ctrl/Cmd+Enter=发送 | 安卓输入法回车误触发发送 |
| `packages/client/connection/src/browser-auth.ts` | loopback 来源跳过浏览器会话 token 鉴权 | 恢复“直接打开 127.0.0.1:3080”的老体验；非 loopback（trusted-host）仍需 token |
| `scripts/install-lefthook.mjs` | Android 上跳过 git hooks 安装 | lefthook 无 android-arm64 二进制，否则 `pnpm install` 整体失败 |
| `docs/termux/**`、`README.md` | Termux 安装/构建/运行/沙箱文档与 fork 声明 | 本分支的使用入口 |

**0.2 升级后已不再需要的旧适配**（上游已自带或该文件已删除）：

- `agent-presets` 的 `code` → `ptc` 兼容别名：0.2 已原生完成改名。
- `ui-chat` 的 `StatsLine` 手机端换行：该文件在 0.2 已被删除，统计行改为官方实现。
- 自建 `tool-plugin-market` 扩展：0.2 官方 `plugin_manager` 工具已覆盖同等能力。
| `docs/termux/README.md` | 本文档 | 适配说明 |
| `docs/termux/setup-termux.sh` | 一键环境配置脚本 | 自动完成安装/编译/构建 |
| `docs/termux/koffi-android.patch` | koffi bionic 编译补丁 | 见 §3.1 |
| `docs/termux/ripgrep-bridge/` | rg 桥接模块（2 文件） | 见 §3.3 |
| `docs/termux/start-dsh-web.sh` | 一键启动脚本 | 见 §5.2 |
| `docs/termux/sandbox/` | 安卓原生沙箱 runner（C 源码 + 安装/自检脚本 + 说明） | 上游 `bwrap`/Landlock 在 Android 不可用，补上 `read-only`/`workspace-write`；**不修改任何上游源码**，见 §3.6 |

### 移动端界面说明

- 手机宽度（≤768px）下聊天区占满全屏；
- 左上角悬浮 `☰` 按钮打开侧栏抽屉（会话列表、新建会话）；
- 点击消息打开详情面板，从右侧滑出；
- 底部统计行（输入/输出 token、缓存命中率）自动换行，完整可见。

---

## 7. 锁文件策略与更新维护

- 仓库中提交**官方原版锁文件**（v9.0，pnpm 9/11 通用格式）；
- Termux 用户本地用 pnpm 9 安装时，pnpm 9 会把锁文件做一次**本地规范化**
  （specifier 写法 `workspace:^`/`link:` 互换、overrides/patchedDependencies
  位置调整），内容等价——**这是本地改动，请勿提交**，与官方保持同步；
- 从上游拉取更新时，冲突只会出现在本分支改动的少数文件上；
  koffi 补丁、ripgrep 桥接等位于 `node_modules`（不入库），升级后按 §3 重新应用即可。
- **跟随官方更新是安全的**：§3.6 的沙箱全部位于 `docs/termux/`（上游仓库没有这个
  目录），且只通过用户 profile 的配置补丁（`runnerCommand`）接入，没有改动任何
  harness 源码或上游接口。合入上游更新后 `docs/termux/**` 原样保留，必要时重跑一次
  `bash docs/termux/sandbox/install.sh` 即可（它会重新编译并保持接线幂等）。

---

## 8. 已知限制

- **Android 存储隔离**：`/storage/emulated/0` 下的其他应用目录写入受限
  （scoped storage）。设置脚本会询问是否运行 `termux-setup-storage` 授权
  共享存储访问；需要更广的"所有文件访问"时，Termux 0.118.3+ 已声明
  `MANAGE_EXTERNAL_STORAGE`，可在系统设置中单独授予。即便如此，
  建议把项目放在 Termux 自己的目录（如 `~/projects`）下工作；
- **后台冻结**：息屏一段时间后 Android Doze 可能冻结 Termux 进程，解锁手机后恢复；
  重启手机后需重新启动服务；
- **沙箱不可用**：见 §5.1，只能以 `danger-full-access` 运行；
- **网络**：nodejs.org 可能不可达（用本地 `--nodedir` 规避）、github.com 可能不可达
  （可用镜像或 api 下载 tarball），npm 请使用 npmmirror 镜像。

---

## 9. 许可

本分支基于 [DeepSeek Harness](https://github.com/deepseek-ai/deepseek-harness)
（MIT License），保留上游 `LICENSE` 并追加本分支版权行（`Copyright (c) 2026 ihman`）。

本项目为社区独立适配，**与 DeepSeek 官方无任何关联、未获其授权或背书**；
"DeepSeek"、"DeepSeek Harness" 为各自所有者之商标，本分支仅作指代性引用。
