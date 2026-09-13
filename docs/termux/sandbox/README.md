# dsh-termux 安卓原生沙箱（dsh-termux-sandbox）

一个约 16KB 的原生 runner，让 Termux 上的 DeepSeek Harness 拥有和电脑端
一致的 `read-only` / `workspace-write` 文件效果拦截，而**不引入虚拟机、容器、
root、daemon 或额外的常驻内存**。

## 为什么需要它

harness 的 Linux 沙箱后端是两个同内核机制，在 Android 上都不可用：

| 后端 | 需要的条件 | 本机实测 |
| --- | --- | --- |
| bubblewrap (`bwrap`) | 非特权 user namespace | `unshare --user` → `Invalid argument`，无 `/proc/<pid>/uid_map`；Termux 官方仓库也没有 `bwrap` 包 |
| Landlock | Linux ≥ 5.13 且内核编译时开启 `CONFIG_SECURITY_LANDLOCK` | 本机为 Android 5.10 内核，早于 5.13 |

没有可用后端时 harness 会 fail-closed：`read-only` / `workspace-write` 直接报
`SANDBOX_UNAVAILABLE`，只剩 `danger-full-access` 能跑。`dsh-termux-sandbox`
补上的就是这一格。

## 工作原理

Android 不允许应用安装带 `SECCOMP_FILTER_FLAG_NEW_LISTENER` 之外的额外
seccomp 过滤吗？允许——只要先设置 `PR_SET_NO_NEW_PRIVS`。runner 就利用这一点：

1. 给被包装的命令安装一张 seccomp 过滤器；
2. 只有**带写入意图的系统调用**（`openat` 的写标志、`unlinkat`、`renameat`、
   `mkdirat`、`symlinkat`、`utimensat`、`ftruncate`、`fchmod` …）会触发
   `SECCOMP_RET_USER_NOTIF` 通知；纯读取调用完全不过监督进程，保持原生速度；
3. runner 作为监督进程读出被拦截系统调用的路径（含 `dirfd`/`fd` 相对路径、
   符号链接解析、`rename` 两端、`utimensat(fd, NULL, …)` 这类 fd 目标），
   按策略判定 `allow` / `EACCES`；
4. 判定发生在内核系统调用返回之前，所以「拒绝就是拒绝」——被拒的写入不可能
   已经落盘；一个系统调用一次判定，没有批处理窗口。

策略来源就是 harness 已有的 bwrap 风格 profile：

```text
dsh-termux-sandbox --ro-bind / / --dev /dev --unshare-pid --proc /proc \
  --die-with-parent [--tmpfs /tmp] [--bind <workspace> <workspace>] -- <command> [args...]
```

- `read-only`（只有 `--ro-bind`）：除设备节点与 `/proc/self/*` 外，一律只读；
- `workspace-write`：额外允许 `--bind`（会话工作区）与 `--tmpfs`（临时区）；
- `danger-full-access`：harness 根本不调用 runner，命令原生执行。

`--unshare-pid` / `--dev` / `--proc` 只是被记录并忽略：Android 应用没有
`CAP_SYS_ADMIN`，无法真正 unshare 或挂载；沙箱的承诺范围（文件效果）不涉及
进程可见性，这一点与 harness 的沙箱词汇表一致。

## 与电脑端一致的功能

- 三种模式在网页端权限选择器里可切换，`read-only` / `workspace-write` 有真实
  拦截，不再因为「无后端」而报错；
- 被拒绝的命令 stderr 里带 `Permission denied`（EACCES，与 Landlock 同方言），
  harness 照常显示 `[sandbox: file access denied under <mode> mode]`；
- 模型可在同一轮次用 `sandbox_permissions` + `justification` 申请更宽模式，网页端
  弹出审批；批准前不会执行；
- 文件写入/编辑工具由 `dsh-fs-sandbox` 在进程内围栏，和 runner 用同一份
  `sandbox-policy`，两者不会漂移。

## 边界之外：IPC 与其它进程

沙箱锁的是**本进程族发起文件写入的那一次系统调用**。设备节点（`/dev/tty`、
`/dev/binder` 等）在允许列表内，所以终端与 Android IPC 都能正常工作；但如果一个
客户端只是把命令交给**沙箱之外**的另一个进程或系统服务去执行，真正的写入发生在
那一侧，本 runner 管不到——那些通道由它们自己的授权机制约束。

因此：需要经由其它进程/服务落地写入的工作流，请把会话切到 `danger-full-access`
（电脑端「不设防」那一档的等价物）；只要目标是限制 Termux 进程直接写宿主文件，
`read-only` / `workspace-write` 就够用。

## 安装

```bash
bash docs/termux/sandbox/install.sh          # 默认接线到 web profile
bash docs/termux/sandbox/install.sh tui      # 或指定其它 profile
```

脚本会编译到 `$PREFIX/bin/dsh-termux-sandbox`，做一次「只读必须挡住写入」自检，
然后在 `~/.dsh/profiles/<profile>/cordis.patch.yml` 里补一条：

```yaml
- id: sandbox
  config:
    runnerCommand:
      - /data/data/com.termux/files/usr/bin/dsh-termux-sandbox
    runnerFailureSignatures:
      - 'dsh-termux-sandbox: fatal:'
```

重启 dsh 即可。`docs/termux/start-dsh-web.sh` 检测到 runner 后会默认
`workspace-write` + 审批；否则退回 `danger-full-access` 并提示安装。

## 调试

| 环境变量 | 作用 |
| --- | --- |
| `DSH_TERMUX_SANDBOX_DEBUG=1` | 把每条被判定的系统调用、路径与结论打到 stderr |
| `DSH_TERMUX_SANDBOX_ALLOW=/a:/b` | 在策略之外追加可写路径（冒号分隔，绝对路径） |
| `DSH_TERMUX_SANDBOX_GRACE_MS=50` | 命令退出后监督进程的收尾轮询间隔 |

runner 自身的故障一律以 `dsh-termux-sandbox: fatal:` 开头并退出 125，这样
harness 会把它归类为「沙箱基础设施故障」而不是「命令被拒」。

## 已知限制（刻意保留的边界）

- **合作式边界，不是对抗式监狱**：路径判定在用户态完成，被包装进程如果恶意地
  并发改写符号链接，理论上存在 TOCTOU 窗口。它挡的是模型越界写文件，不是
  受过训练的本地攻击者。真正需要对抗性隔离时请用 `danger-full-access` 之外的
  外部手段（另一台机器 / 云端沙箱）。
- **无 PID / 网络 / IPC 隔离**：与 harness 沙箱词汇表的承诺范围一致（那些不属于
  `SandboxMode`）。
- **逃逸面已封堵但非穷尽**：`ptrace`、`process_vm_writev`、`mount`、`setns`、
  `unshare`、`bpf`、`keyctl`、内核模块加载、`io_uring_setup`、`name_to_handle_at`
  等一律 `EACCES`；新增内核接口需要同步更新。
- **后台进程与 `setsid`**：非交互 shell 的 `&` 后台任务仍在命令进程组内，会被继续
  监督；主动 `setsid` 脱离的进程在监督进程收尾后写入会得到 `ENOSYS`（失败，不会
  变成未拦截的写入）。长任务请用 `run_in_background`。
- **`/tmp` 映射**：Android 根目录对应用只读，无法创建真正的 `/tmp`；runner 会把
  `--tmpfs /tmp` 同时映射到 `$TMPDIR`（Termux 为 `$PREFIX/tmp`），Termux 工具链
  默认遵守该变量。

## 卸载

```bash
rm -f "$PREFIX/bin/dsh-termux-sandbox"
# 再从 ~/.dsh/profiles/<profile>/cordis.patch.yml 删掉那条 id: sandbox 覆盖
```
