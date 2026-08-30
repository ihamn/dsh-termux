#!/bin/sh
# DeepSeek Harness 0.1.2-alpha.1 — Termux 一键环境配置脚本
#
# 用法:
#   bash docs/termux/setup-termux.sh
#
# 按顺序完成：工具链安装 → pnpm 9 → 依赖安装 → 原生模块编译/桥接 → 构建。
# 参考环境：Termux v0.119.0-beta.3 / Android 14 / aarch64（见 README.md §1.1）。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

echo "============================================================"
echo " DeepSeek Harness 0.1.2-alpha.1 — Termux 一键环境配置"
echo "------------------------------------------------------------"
echo " 本脚本将依次执行："
echo "   1. pkg install 工具链（nodejs-lts / cmake / clang / ...）"
echo "   2. 全局安装 pnpm 9.15.9"
echo "      （原因：pnpm 10/11 在 Android 上因自身二进制自校验"
echo "        拒绝执行 install/run；9.x 无该校验且原生支持 v9 锁文件）"
echo "   3. pnpm install 安装全部依赖"
echo "      （官方 v9 锁文件，无需改写）"
echo "   4. 源码编译 koffi / node-pty 等原生模块"
echo "   5. 配置 ripgrep 桥接与 sharp WebAssembly 运行时"
echo "   6. 构建服务端、客户端插件与网页前端"
echo "------------------------------------------------------------"
echo " 预计额外占用约 2GB 存储，耗时 15-30 分钟，需要网络。"
echo " 参考环境：Termux v0.119.0-beta.3 / Android 14 / aarch64"
echo "------------------------------------------------------------"
echo " 权限与风险确认："
echo "   1. 本脚本会【全局安装】pnpm 9.15.9（npm install -g），"
echo "      影响 Termux 的全局 Node 环境；"
echo "   2. 项目运行时以 danger-full-access（无沙箱）模式运行，"
echo "      agent 可读写 Termux 权限范围内的文件，请仅在需要时启动；"
echo "   3. 安装过程中会询问是否授予手机共享存储访问权限"
echo "      （/storage/emulated/0，将弹出系统授权窗口）；"
echo "   4. 本项目为社区适配分支，与 DeepSeek 官方无任何关联。"
echo "============================================================"
printf "确认继续? [y/N] "
read -r CONFIRM
case "$CONFIRM" in
  y|Y|yes|YES) ;;
  *) echo "已取消，未做任何修改。"; exit 1 ;;
esac

printf "是否现在授予手机共享存储访问权限（将弹出系统授权窗口）? [y/N] "
read -r STORAGE_CONFIRM
case "$STORAGE_CONFIRM" in
  y|Y|yes|YES)
    if command -v termux-setup-storage >/dev/null 2>&1; then
      echo "==> 授予共享存储访问权限..."
      termux-setup-storage
    else
      echo "警告: 未找到 termux-setup-storage，请稍后手动运行"
    fi
    ;;
  *)
    echo "已跳过共享存储授权；如需访问 /storage/emulated/0，请稍后运行 termux-setup-storage"
    ;;
esac
echo

echo "==> [1/8] 安装 Termux 基础工具链"
pkg install -y nodejs-lts cmake make clang binutils ndk-sysroot python ripgrep git patch

echo "==> [2/8] 安装 pnpm 9.15.9（pnpm 10/11 在 Android 上有二进制校验问题）"
if command -v pnpm >/dev/null 2>&1 && [ "$(pnpm --version 2>/dev/null | cut -d. -f1)" = "9" ]; then
  echo "pnpm 9 已就绪: $(pnpm --version)"
else
  # npmmirror 的 cdn 偶发超时；npmjs / 华为镜像实测可直连
  NPM_REGISTRY="https://mirrors.huaweicloud.com/repository/npm/"
  if curl -s -o /dev/null --max-time 8 https://registry.npmjs.org/pnpm; then
    NPM_REGISTRY="https://registry.npmjs.org"
  fi
  npm install -g pnpm@9.15.9 --registry="$NPM_REGISTRY"
fi

echo "==> [3/8] 安装依赖（锁文件会被 pnpm 9 本地规范化，属本地改动，请勿提交）"
pnpm install --registry="${NPM_REGISTRY:-https://registry.npmjs.org}"

echo "==> [4/8] 编译 koffi（应用 Android bionic 补丁）"
KOFFI_DIR="$(echo node_modules/.pnpm/koffi@*/node_modules/koffi | awk '{print $1}')"
if [ -d "$KOFFI_DIR" ]; then
  (cd "$KOFFI_DIR" && patch -N -p1 < "$ROOT/docs/termux/koffi-android.patch" || true)
  (cd "$KOFFI_DIR" && node ./cnoke.cjs -P . -D src/koffi --prebuild --release)
else
  echo "警告: 未找到 koffi 包目录，跳过"
fi

echo "==> [5/8] 编译 node-pty（使用 Termux 本地头文件，避免 nodejs.org 下载）"
NODE_PTY_DIR="$(echo node_modules/.pnpm/node-pty@*/node_modules/node-pty | awk '{print $1}')"
if [ -d "$NODE_PTY_DIR" ]; then
  (cd "$NODE_PTY_DIR" && node "$(npm root -g)/npm/node_modules/node-gyp/bin/node-gyp.js" rebuild --nodedir="$PREFIX")
  # spawn-helper 需要可执行位（官方 ensure-spawn-helper 只处理已存在文件）
  find "$NODE_PTY_DIR/build" -name spawn-helper -exec chmod +x {} \; 2>/dev/null || true
else
  echo "警告: 未找到 node-pty 包目录，跳过"
fi

echo "==> [6/8] 安装 ripgrep 桥接（glob/grep 工具）"
mkdir -p node_modules/@vscode/ripgrep
cp docs/termux/ripgrep-bridge/package.json docs/termux/ripgrep-bridge/index.js node_modules/@vscode/ripgrep/

echo "==> [7/8] 安装 sharp WebAssembly 运行时"
if [ ! -d node_modules/@img/sharp-wasm32 ]; then
  echo "sharp-wasm32 未随 pnpm install 就位，手动安装..."
  TMP="$(mktemp -d)"
  (cd "$TMP" && npm install @img/sharp-wasm32@0.35.3 --registry=https://registry.npmmirror.com)
  mkdir -p node_modules/@img
  cp -r "$TMP/node_modules/@img/sharp-wasm32" node_modules/@img/
  for d in @emnapi tslib; do
    if [ -d "$TMP/node_modules/$d" ]; then cp -r "$TMP/node_modules/$d" node_modules/; fi
  done
  rm -rf "$TMP"
else
  echo "sharp-wasm32 已就绪"
fi

echo "==> [8/8] 构建（服务端 + 客户端插件 + 网页前端）"
pnpm run build:lib:host
pnpm run build:lib:client
pnpm --filter @deepseek-ai/dsh-web-frontend run build

echo
echo "==> 可选：配置 DeepSeek API Key"
printf "是否现在配置 DeepSeek API Key（写入 ~/.bashrc，输入不回显）? [y/N] "
read -r KEY_CONFIRM
case "$KEY_CONFIRM" in
  y|Y|yes|YES)
    printf "请输入 DeepSeek API Key: "
    stty -echo 2>/dev/null || true
    read -r DEEPSEEK_KEY
    stty echo 2>/dev/null || true
    printf "\n"
    if [ -n "$DEEPSEEK_KEY" ]; then
      if grep -q "DEEPSEEK_API_KEY=" "$HOME/.bashrc" 2>/dev/null; then
        sed -i "s|^export DEEPSEEK_API_KEY=.*|export DEEPSEEK_API_KEY=$DEEPSEEK_KEY|" "$HOME/.bashrc"
      else
        printf '\nexport DEEPSEEK_API_KEY=%s\n' "$DEEPSEEK_KEY" >> "$HOME/.bashrc"
      fi
      echo "✅ 已写入 ~/.bashrc"
    else
      echo "未输入，跳过"
    fi
    ;;
  *)
    echo "跳过；可稍后在网页端 Settings → Models 配置第三方提供商，"
    echo "或在启动前手动执行: export DEEPSEEK_API_KEY=sk-xxx"
    ;;
esac

echo
echo "==============================================="
echo "✅ 全部完成！启动："
echo "  bash docs/termux/start-dsh-web.sh            # 前台"
echo "  bash docs/termux/start-dsh-web.sh --bg       # 后台 + wake-lock"
echo "  浏览器打开 http://127.0.0.1:3080"
echo "==============================================="
