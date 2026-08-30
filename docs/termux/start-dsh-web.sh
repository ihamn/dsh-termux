#!/bin/sh
# 启动 DeepSeek Harness 网页版（Termux / Android）
#
# 用法:
#   bash docs/termux/start-dsh-web.sh                  # 前台运行
#   bash docs/termux/start-dsh-web.sh --bg             # 后台运行 + wake-lock
#   bash docs/termux/start-dsh-web.sh -- --host 0.0.0.0  # 局域网可访问
#
# 需要先设置 API key:
#   export DEEPSEEK_API_KEY=sk-xxx

# Termux 没有沙箱后端，默认不设防；需要收紧可覆盖该变量
: "${DSH_PERMISSION_MODE:=danger-full-access}"
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
  echo "dsh web 已在后台启动 （日志: ~/dsh-web.log）"
  # 新版默认启用 token 鉴权；从日志中提取带 token 的访问地址
  TOKEN_URL="$(grep -m1 -o 'http://127.0.0.1:3080/?token=[A-Za-z0-9_-]*' ~/dsh-web.log 2>/dev/null || true)"
  if [ -n "$TOKEN_URL" ]; then
    echo "访问地址: $TOKEN_URL"
  else
    echo "访问地址: http://127.0.0.1:3080 （若要求 token，请查看 ~/dsh-web.log）"
  fi
  echo "停止: pkill -f '^node --expose-internals apps/cli/lib/bin.js'"
  exit 0
fi

exec node --expose-internals apps/cli/lib/bin.js --profile web "$@"
