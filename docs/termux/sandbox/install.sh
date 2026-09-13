#!/bin/sh
# 安装 dsh-termux 安卓原生沙箱 runner，并把它接到某个 dsh profile 上。
#
# 用法:
#   bash docs/termux/sandbox/install.sh            # 默认 web profile
#   bash docs/termux/sandbox/install.sh tui        # 指定 profile
#
# 做两件事:
#   1. 用 Termux 自带 clang 编译 docs/termux/sandbox/dsh-termux-sandbox.c
#      到 $PREFIX/bin/dsh-termux-sandbox（约 16KB，无守护进程、无 root、无 VM）；
#   2. 在 ~/.dsh/profiles/<profile>/cordis.patch.yml 里追加一条 sandbox 覆盖，
#      把 @deepseek-ai/dsh-sandbox-local 的 runner 指到这个二进制。
#
# 之后 read-only / workspace-write 两种模式就有了真正的内核级文件效果拦截，
# 权限预设切换、拒绝后的升权审批链路与电脑上完全一致；danger-full-access
# 仍然是不经 runner 的原生放行。

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
PROFILE="${1:-web}"
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
BIN="$PREFIX/bin/dsh-termux-sandbox"
PATCH_FILE="$HOME/.dsh/profiles/$PROFILE/cordis.patch.yml"

if ! command -v clang >/dev/null 2>&1; then
  echo "缺少 clang，请先执行: pkg install clang" >&2
  exit 1
fi

echo "==> 编译 $BIN"
mkdir -p "$PREFIX/bin"
clang -O2 -Wall -Wextra -o "$BIN" "$HERE/dsh-termux-sandbox.c"
chmod 700 "$BIN"

echo "==> 自检：只读模式必须拦截写入"
PROBE_DIR="${TMPDIR:-$PREFIX/tmp}/dsh-termux-sandbox-probe.$$"
if "$BIN" --ro-bind / / --dev /dev --unshare-pid --proc /proc --die-with-parent \
    -- sh -c "touch '$PROBE_DIR'" >/dev/null 2>&1; then
  rm -f "$PROBE_DIR"
  echo "自检失败：只读模式没有拦住写入，请把上面的编译警告反馈给项目。" >&2
  exit 1
fi
if [ -e "$PROBE_DIR" ]; then
  rm -f "$PROBE_DIR"
  echo "自检失败：只读模式仍创建了文件。" >&2
  exit 1
fi
echo "    只读拦截生效"

if [ ! -f "$PATCH_FILE" ]; then
  echo "==> 创建 $PATCH_FILE"
  mkdir -p "$(dirname "$PATCH_FILE")"
  printf '[]\n' > "$PATCH_FILE"
fi

if grep -q 'dsh-termux-sandbox' "$PATCH_FILE"; then
  echo "==> $PATCH_FILE 已接线，跳过"
else
  echo "==> 接线到 $PATCH_FILE"
  # 空数组占位符 `[]` 与真实条目不能共存，先摘掉它（注释头保留）。
  PATCH_TMP="$PATCH_FILE.tmp.$$"
  grep -v '^[[:space:]]*\[\][[:space:]]*$' "$PATCH_FILE" > "$PATCH_TMP"
  {
    printf '\n'
    printf '%s\n' '- id: sandbox'
    printf '%s\n' '  config:'
    printf '%s\n' '    runnerCommand:'
    printf '      - %s\n' "$BIN"
    printf '%s\n' '    runnerFailureSignatures:'
    printf '%s\n' "      - 'dsh-termux-sandbox: fatal:'"
  } >> "$PATCH_TMP"
  mv "$PATCH_TMP" "$PATCH_FILE"
fi

echo
echo "完成。重启 dsh 后，权限选择器里的三种模式都会生效："
echo "  read-only          所有写入被拒（含 chmod/rename/ptrace 等逃逸面），拒绝即可升权申请"
echo "  workspace-write    只有会话工作区与 \$TMPDIR 可写"
echo "  danger-full-access 不经过 runner 的原生放行（需要完全放行时用这一档）"
echo
echo "默认档位由 DSH_PERMISSION_MODE 决定；docs/termux/start-dsh-web.sh 在检测到"
echo "本 runner 后会默认使用 workspace-write + 审批，而不是 danger-full-access。"
