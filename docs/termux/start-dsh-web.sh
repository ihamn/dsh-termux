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
  echo "dsh web 正在后台启动 （日志: ~/dsh-web.log）"
  # 新版默认启用 token 鉴权；轮询日志直到服务打印出带 token 的访问地址
  # （服务冷启动约需数秒，一次 2 秒 sleep 经常抓不到）
  TOKEN_URL=""
  i=0
  while [ "$i" -lt 30 ]; do
    TOKEN_URL="$(grep -m1 -o 'http://127.0.0.1:3080/?token=[A-Za-z0-9_-]*' ~/dsh-web.log 2>/dev/null || true)"
    [ -n "$TOKEN_URL" ] && break
    sleep 1
    i=$((i + 1))
  done
  if [ -n "$TOKEN_URL" ]; then
    echo "访问地址: $TOKEN_URL"
    echo "提示: 首次打开后浏览器会记住授权（约 30 天）；服务重启后 token 会变，请重新打开上面地址。"
  else
    echo "访问地址: http://127.0.0.1:3080 （若要求 token，请查看 ~/dsh-web.log 中的访问地址）"
  fi
  echo "停止: pkill -f '^node --expose-internals apps/cli/lib/bin.js'"
  exit 0
fi

exec node --expose-internals apps/cli/lib/bin.js --profile web "$@"
