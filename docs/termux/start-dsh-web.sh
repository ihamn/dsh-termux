#!/bin/sh
# 启动 DeepSeek Harness 网页版（Termux / Android）
#
# 用法:
#   pwd; ls -la                                        # 先确认当前目录与脚本都在
#   bash start-dsh-web.sh --bg                         # 后台运行 + wake-lock
#   bash docs/termux/start-dsh-web.sh                  # 或在仓库里前台运行
#   bash docs/termux/start-dsh-web.sh -- --trusted-host <手机IP>   # 局域网可访问
#
# 需要先设置 API key:
#   export DEEPSEEK_API_KEY=sk-xxx
#
# 工作区：脚本会 cd 到仓库根，process.cwd() 即新会话的默认工作区
# （也是 workspace-write 允许写入的根）；要换工作区请在该目录下直接运行
#   node --expose-internals <仓库>/apps/cli/lib/bin.js --profile web

# 权限预设：装上 docs/termux/sandbox 的原生 runner 后，默认走
# workspace-write + 审批（拒绝后可升权申请，与电脑端一致）；
# 没有 runner 时退回 danger-full-access（旧行为）。
# 无论哪种情况，都可以用 DSH_PERMISSION_MODE=read-only 等显式覆盖。
if [ -z "${DSH_PERMISSION_MODE:-}" ]; then
  if command -v dsh-termux-sandbox >/dev/null 2>&1; then
    DSH_PERMISSION_MODE=workspace-write
  else
    DSH_PERMISSION_MODE=danger-full-access
    echo "提示: 未检测到 dsh-termux-sandbox，本次以 danger-full-access 运行（无沙箱）。"
    echo "      启用安卓原生只读/工作区沙箱: bash docs/termux/sandbox/install.sh"
  fi
fi
export DSH_PERMISSION_MODE

# 定位仓库根：本脚本位于 docs/termux/，仓库根在其上两级
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
[ -d "$ROOT/apps/cli" ] || ROOT="$HOME/deepseek-harness"
cd "$ROOT"

if [ "$1" = "--bg" ]; then
  shift
  command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock
  if command -v setsid >/dev/null 2>&1; then
    setsid nohup node --expose-internals apps/cli/lib/bin.js --profile web "$@" > ~/dsh-web.log 2>&1 < /dev/null &
  else
    nohup node --expose-internals apps/cli/lib/bin.js --profile web "$@" > ~/dsh-web.log 2>&1 < /dev/null &
  fi
  sleep 2
  echo "dsh web 已在后台启动: http://127.0.0.1:3080 （日志: ~/dsh-web.log）"
  echo "提示: 本分支已对 loopback 访问免 token，手机浏览器直接打开 http://127.0.0.1:3080 即可。"
  echo "停止: pkill -f '^node --expose-internals apps/cli/lib/bin.js'"
  exit 0
fi

exec node --expose-internals apps/cli/lib/bin.js --profile web "$@"
