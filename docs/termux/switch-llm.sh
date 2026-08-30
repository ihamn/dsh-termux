#!/bin/sh
# switch-llm.sh — 在「OpenCode Zen · DeepSeek V4 Flash Free（免费）」与
# 「DeepSeek 官方 API」之间热切换，无需重启 web 服务。
#
# 用法:
#   ./switch-llm.sh zen        切到 Zen 免费模型（新会话默认生效）
#   ./switch-llm.sh official   切回 DeepSeek 官方 API
#   ./switch-llm.sh status     查看当前生效配置
#
# 原理:
#   harness 的 settings.yaml 与 .credentials.yaml 支持热重载（文件监听 +
#   每请求重新解析），脚本只改写这两个文件：
#     settings.yaml      → agent-default-model（新会话默认模型）+ llm-deepseek（端点/密钥引用/模型目录）
#     .credentials.yaml  → ZEN_API_KEY / DEEPSEEK_API_KEY（仅本地，0600 权限）
#   已有会话保留原模型，新建会话才用新默认；如需立刻切换当前会话，
#   可在会话内用模型选择器选择 deepseek-v4-flash-free。
#
# 注意:
#   1. Zen 免费模型免费期内输入/输出/缓存全免费，但请求数据可能被用于
#      OpenCode 改进模型，请勿提交敏感/机密内容。
#   2. 首次切 zen 会提示输入 Zen API Key（输入不回显，仅写入
#      .credentials.yaml，不会出现在脚本或任何仓库文件里）。
#   3. 官方模式默认读取进程环境中的 DEEPSEEK_API_KEY（即
#      start-dsh-web.sh 启动时的密钥）。

set -eu

DSH_HOME="${DSH_HOME:-$HOME/.dsh}"
SETTINGS="$DSH_HOME/settings.yaml"
CREDENTIALS="$DSH_HOME/.credentials.yaml"

ZEN_MODEL="deepseek-v4-flash-free"
ZEN_NAME="DeepSeek-V4-Flash-Free (Zen)"
ZEN_BASE_URL="https://opencode.ai/zen/v1"
OFFICIAL_MODEL="deepseek-v4-flash"
CONTEXT_WINDOW="1000000"

# ---------- 工具函数 ----------

# 删除 settings.yaml 中的某个顶层分节（key + 其缩进子行），其余内容原样保留
# 省略文件参数时从 stdin 读取
drop_section() {
  if [ "$#" -ge 2 ]; then
    awk -v key="$1" '
      $0 ~ "^" key ":" { skip=1; next }
      skip && $0 ~ /^[^[:space:]]/ { skip=0 }
      skip { next }
      { print }
    ' "$2"
  else
    awk -v key="$1" '
      $0 ~ "^" key ":" { skip=1; next }
      skip && $0 ~ /^[^[:space:]]/ { skip=0 }
      skip { next }
      { print }
    '
  fi
}

# 在 .credentials.yaml 中写入一个密钥（保留其它条目，权限固定 0600）
set_credential() {
  if [ ! -f "$CREDENTIALS" ]; then
    : > "$CREDENTIALS"
    chmod 600 "$CREDENTIALS"
  fi
  chmod 600 "$CREDENTIALS"
  if grep -q "^$1:" "$CREDENTIALS" 2>/dev/null; then
    sed -i "s|^$1:.*|$1: $2|" "$CREDENTIALS"
  else
    printf '%s: %s\n' "$1" "$2" >> "$CREDENTIALS"
  fi
}

# 确保 Zen 密钥已就绪（缺失时静默录入）
ensure_zen_key() {
  if grep -q "^ZEN_API_KEY:" "$CREDENTIALS" 2>/dev/null; then
    return 0
  fi
  printf "还没有配置 Zen API Key。\n"
  printf "请粘贴 OpenCode Zen API Key（输入不回显）: "
  stty -echo 2>/dev/null || true
  read -r ZEN_KEY
  stty echo 2>/dev/null || true
  printf "\n"
  if [ -z "$ZEN_KEY" ]; then
    echo "未输入，已取消。"
    exit 1
  fi
  set_credential "ZEN_API_KEY" "$ZEN_KEY"
  echo "✅ ZEN_API_KEY 已写入 $CREDENTIALS"
}

# 写入 settings.yaml（保留其它分节，替换/追加目标两个分节）
apply_settings() {
  mode="$1"
  tmp="$SETTINGS.tmp"
  : > "$tmp"
  if [ -f "$SETTINGS" ]; then
    drop_section "agent-default-model" "$SETTINGS" \
      | drop_section "llm-deepseek" > "$tmp"
  fi
  if [ "$mode" = "zen" ]; then
    cat >> "$tmp" <<EOF
agent-default-model:
  provider: deepseek-official
  model: $ZEN_MODEL
llm-deepseek:
  baseURL: $ZEN_BASE_URL
  apiKeyEnv: ZEN_API_KEY
  maxTokens: 131072
  models:
    - id: $ZEN_MODEL
      name: "$ZEN_NAME"
      contextWindow: $CONTEXT_WINDOW
      maxTokens: 131072
EOF
  else
    cat >> "$tmp" <<EOF
agent-default-model:
  provider: deepseek-official
  model: $OFFICIAL_MODEL
EOF
  fi
  mv "$tmp" "$SETTINGS"
  chmod 600 "$SETTINGS"
}

# ---------- 命令分发 ----------

case "${1:-}" in
  zen)
    ensure_zen_key
    apply_settings zen
    echo "已切换到 Zen 免费模型：$ZEN_MODEL"
    echo "生效方式：热重载（约 1 秒），无需重启服务。"
    echo "提示：已有会话保留原模型，请新开会话，或在会话内选择 $ZEN_MODEL。"
    ;;
  official)
    apply_settings official
    echo "已切回 DeepSeek 官方 API：$OFFICIAL_MODEL"
    echo "生效方式：热重载（约 1 秒），无需重启服务。"
    echo "密钥来源：进程环境 DEEPSEEK_API_KEY（start-dsh-web.sh 启动时的密钥）。"
    ;;
  status)
    echo "DSH_HOME: $DSH_HOME"
    echo "--- agent-default-model ---"
    if grep -q '^agent-default-model:' "$SETTINGS" 2>/dev/null; then
      awk -v key='agent-default-model' '
        $0 ~ "^" key ":" { show=1; print; next }
        show && $0 ~ /^[^[:space:]]/ { exit }
        show { print }
      ' "$SETTINGS"
    else
      echo "（未配置，使用默认: deepseek-official / deepseek-v4-flash）"
    fi
    echo "--- llm-deepseek ---"
    if grep -q '^llm-deepseek:' "$SETTINGS" 2>/dev/null; then
      awk -v key='llm-deepseek' '
        $0 ~ "^" key ":" { show=1; print; next }
        show && $0 ~ /^[^[:space:]]/ { exit }
        show { print }
      ' "$SETTINGS"
    else
      echo "（未配置，使用默认: 官方端点 + 环境 DEEPSEEK_API_KEY）"
    fi
    echo "--- 已存密钥（仅显示是否存在）---"
    for ref in ZEN_API_KEY DEEPSEEK_API_KEY; do
      if grep -q "^$ref:" "$CREDENTIALS" 2>/dev/null; then
        echo "$ref: 已配置"
      else
        echo "$ref: 未配置"
      fi
    done
    ;;
  *)
    echo "用法: ./switch-llm.sh {zen|official|status}"
    echo "  zen       切到 OpenCode Zen 免费模型 deepseek-v4-flash-free"
    echo "  official  切回 DeepSeek 官方 API（deepseek-v4-flash）"
    echo "  status    查看当前配置"
    exit 1
    ;;
esac
