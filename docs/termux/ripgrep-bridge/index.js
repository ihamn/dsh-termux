// Termux bridge: the real @vscode/ripgrep has no Android prebuilt, so point
// the harness's glob/grep tools at the system ripgrep (pkg install ripgrep).
export const rgPath = '/data/data/com.termux/files/usr/bin/rg'
