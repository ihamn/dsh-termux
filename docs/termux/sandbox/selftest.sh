#!/bin/sh
# dsh-termux-sandbox 自检：验证只读/工作区两种档位的真实拦截行为。
#
# 用法: bash docs/termux/sandbox/selftest.sh [runner 路径]
#
# 覆盖：读取放行、写入拒绝、/dev/null 放行、退出码透传、rename 逃逸被拦、
# 工作区内 chmod/rename/symlink 放行、$TMPDIR 放行、后台任务仍被监督。

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
if [ -n "${1:-}" ]; then
  BIN="$1"
elif [ -x "$HERE/dsh-termux-sandbox" ]; then
  BIN="$HERE/dsh-termux-sandbox"
elif command -v dsh-termux-sandbox >/dev/null 2>&1; then
  BIN="$(command -v dsh-termux-sandbox)"
else
  echo "找不到 dsh-termux-sandbox，请先运行 install.sh 或把路径作为第一个参数传入" >&2
  exit 2
fi
# Both trees live outside $TMPDIR on purpose: workspace-write is allowed to
# write the temp area, so "outside the workspace" must mean outside it too.
WORKDIR="$(mktemp -d "$HOME/dsh-termux-sandbox-test.XXXXXX")"
OUTSIDE="$(mktemp -d "$HOME/dsh-termux-sandbox-outside.XXXXXX")"
PASS=0
FAIL=0

read_only() {
  "$BIN" --ro-bind / / --dev /dev --unshare-pid --proc /proc --die-with-parent -- "$@"
}
workspace_write() {
  "$BIN" --ro-bind / / --dev /dev --unshare-pid --proc /proc --die-with-parent \
    --tmpfs /tmp --bind "$WORKDIR" "$WORKDIR" -- "$@"
}
check() {
  if [ "$2" = "$3" ]; then
    PASS=$((PASS + 1)); printf '  ok   %s\n' "$1"
  else
    FAIL=$((FAIL + 1)); printf '  FAIL %s (期望 %s，实际 %s)\n' "$1" "$3" "$2"
  fi
}

echo "runner: $BIN"
echo "只读档:"
echo data > "$WORKDIR/readable"
check "读取放行" "$(read_only cat "$WORKDIR/readable" 2>/dev/null)" "data"
check "写入拒绝" "$(read_only sh -c "touch $WORKDIR/blocked" >/dev/null 2>&1; echo $?)" "1"
check "写入未落盘" "$([ -e "$WORKDIR/blocked" ] && echo created || echo absent)" "absent"
check "/dev/null 放行" "$(read_only sh -c 'echo x >/dev/null && echo ok' 2>/dev/null)" "ok"
check "退出码透传" "$(read_only sh -c 'exit 7' 2>/dev/null; echo $?)" "7"
check "rename 逃逸被拦" \
  "$(read_only sh -c "mv $WORKDIR/readable $OUTSIDE/stolen" >/dev/null 2>&1; echo $?)" "1"
check "源文件仍在" "$([ -e "$WORKDIR/readable" ] && echo present || echo gone)" "present"

echo "工作区档:"
check "工作区写入放行" "$(workspace_write sh -c "echo hi > $WORKDIR/w && cat $WORKDIR/w" 2>/dev/null)" "hi"
check "工作区 chmod 放行" "$(workspace_write chmod 600 "$WORKDIR/w" >/dev/null 2>&1; echo $?)" "0"
check "工作区 rename 放行" "$(workspace_write sh -c "mkdir -p $WORKDIR/d && mv $WORKDIR/w $WORKDIR/d/w" >/dev/null 2>&1; echo $?)" "0"
check "工作区 symlink 放行" "$(workspace_write ln -s "$WORKDIR/d/w" "$WORKDIR/l" >/dev/null 2>&1; echo $?)" "0"
check "TMPDIR 放行" "$(workspace_write sh -c 'f=$(mktemp) && echo ok >"$f" && echo ok' 2>/dev/null)" "ok"
check "工作区外写入拒绝" "$(workspace_write sh -c "touch $OUTSIDE/nope" >/dev/null 2>&1; echo $?)" "1"
check "后台任务仍被监督" \
  "$(workspace_write sh -c "(sleep 1; touch $WORKDIR/bg) & echo started" >/dev/null 2>&1; sleep 1.5; [ -e "$WORKDIR/bg" ] && echo yes || echo no)" "yes"

rm -rf "$WORKDIR" "$OUTSIDE"
echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" -eq 0 ]
