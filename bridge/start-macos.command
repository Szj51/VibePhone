#!/bin/zsh -l
cd -- "${0:A:h}" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
if ! command -v node >/dev/null 2>&1; then
  print '需要 Node.js 20 或更新版本：https://nodejs.org/'
  read '?按回车关闭…'
  exit 1
fi
node bridge.mjs
if [[ $? -ne 0 ]]; then read '?按回车关闭…'; fi
