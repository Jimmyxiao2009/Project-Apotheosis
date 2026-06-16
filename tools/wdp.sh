#!/usr/bin/env bash
# ============================================================================
# wdp.sh — 在 macOS 上通过 Windows Device Portal (WDP) 操作 Lumia 950 (Win10M)
#   部署 appx / 抓诊断文件(stage.txt)/ 抓崩溃 dump。
#
# 协议照搬已在本设备验证可用的 PowerShell 工具(Deploy-Robust.ps1 / Wdp-Crash.ps1):
#   - HTTPS + 自签证书(-k 忽略校验)
#   - CSRF:先 GET 拿 Set-Cookie 里的 CSRF-Token,POST/DELETE 时带 X-CSRF-Token 头 + cookie
#   - 本设备 WDP 未开 Basic 认证;若你的开了,设 WDP_AUTH=user:pass
#   - 更新部署只需传 .appx + .cer(框架依赖首次装机时已在设备上)
#
# 前提:手机和这台 Mac 在同一网络;appx/cer 已拷到 Mac(从 Windows 编译机传过来)。
#
# 用法:
#   export WDP_IP=192.168.x.x            # 手机在「设备门户」里显示的 IP(从 Mac 能 ping 通的那个)
#   ./wdp.sh info                        # 测连通 + 显示设备信息 + 已装 Harness 包名
#   ./wdp.sh deploy ~/Downloads/Harness_0.1.0.9_ARM.appx   # 部署(同目录自动找 .cer)
#   ./wdp.sh diag                        # 抓 LocalState\stage.txt 并打印(SPA/白屏诊断)
#   ./wdp.sh pull "LocalState\\downloads.tsv" out.tsv      # 抓任意 app 文件
#   ./wdp.sh dump-on                     # 开启崩溃 dump 收集(装好后开一次)
#   ./wdp.sh dump                        # 列出并下载最新崩溃 dump 到 ./crash/
#
# 端口默认 443;如设备用 http/其它端口,设 WDP_SCHEME=http WDP_PORT=80。
# ============================================================================
set -u

IP="${WDP_IP:-}"
SCHEME="${WDP_SCHEME:-https}"
# 端口:显式 WDP_PORT 优先;否则 http→80 / https→443
if [ -n "${WDP_PORT:-}" ]; then PORT="$WDP_PORT"; elif [ "$SCHEME" = "http" ]; then PORT=80; else PORT=443; fi
AUTH="${WDP_AUTH:-}"                 # 形如 user:pass;本设备不需要,留空
PKG_MATCH="EdgeHTMLReborn.Harness"  # 目标包匹配子串
COOKIES="$(mktemp /tmp/wdp_cookies.XXXXXX)"
trap 'rm -f "$COOKIES"' EXIT

if [ -z "$IP" ]; then
  echo "✗ 先设手机 IP:  export WDP_IP=192.168.x.x   (在手机「设置→更新和安全→面向开发人员→设备门户」里看)" >&2
  exit 2
fi
BASE="$SCHEME://$IP:$PORT"
CURL_AUTH=()
[ -n "$AUTH" ] && CURL_AUTH=(-u "$AUTH")

# 公共 curl:忽略证书、带 cookie jar、合理超时
cput() { curl -sk --connect-timeout 15 "${CURL_AUTH[@]}" -b "$COOKIES" -c "$COOKIES" "$@"; }

# 刷新 CSRF token:GET 一下,从 cookie jar 取 CSRF-Token 值(Netscape 格式,值是最后一列)
csrf() {
  cput "$BASE/api/os/info" >/dev/null 2>&1 || true
  awk '/CSRF-Token/{t=$NF} END{print t}' "$COOKIES"
}

# URL 编码反斜杠(app 文件相对路径用 \ 分隔)
enc_path() { printf '%s' "$1" | sed 's/\\/%5C/g; s/ /%20/g'; }

# 取已装目标包的 PackageFullName(多版本取最后一个)
get_pfn() {
  cput "$BASE/api/app/packagemanager/packages" \
    | tr ',{}' '\n\n\n' \
    | grep -o "\"PackageFullName\":\"[^\"]*${PKG_MATCH}[^\"]*\"" \
    | sed 's/.*"PackageFullName":"//; s/"$//' \
    | sort | tail -1
}

cmd="${1:-help}"; shift || true

case "$cmd" in

info)
  echo "WDP: $BASE"
  info="$(cput "$BASE/api/os/info")" || { echo "✗ 连不上设备门户。检查:同网络?IP 对?端口($PORT)对?" >&2; exit 1; }
  echo "设备: $info"
  pfn="$(get_pfn)"
  if [ -n "$pfn" ]; then echo "已装 Harness: $pfn"; else echo "⚠ 未查到 $PKG_MATCH(还没装过?)"; fi
  ;;

deploy)
  APPX="${1:-}"
  if [ -z "$APPX" ]; then
    # 没给路径:在当前目录找最高版本
    APPX="$(ls -1 Harness_*_ARM.appx 2>/dev/null | sort -V | tail -1)"
  fi
  [ -z "$APPX" ] || [ ! -f "$APPX" ] && { echo "✗ 找不到 appx。用法: ./wdp.sh deploy <path-to-appx>" >&2; exit 2; }
  DIR="$(cd "$(dirname "$APPX")" && pwd)"
  NAME="$(basename "$APPX")"
  CER="$(ls -1 "$DIR"/*.cer 2>/dev/null | head -1)"
  echo "部署: $NAME"
  [ -n "$CER" ] && echo "证书: $(basename "$CER")" || echo "⚠ 同目录无 .cer(更新装通常没问题,首次装可能需要)"

  TOK="$(csrf)"
  echo "CSRF: ${TOK:0:12}…"

  # 多段上传(部分文件名=表单字段名,与 PowerShell 版一致),失败重试 4 次(连接易闪断)
  PARTS=(-F "${NAME}=@${APPX};type=application/octet-stream")
  [ -n "$CER" ] && PARTS+=(-F "$(basename "$CER")=@${CER};type=application/octet-stream")
  sent=0
  for try in 1 2 3 4; do
    echo "上传安装中(~44MB,尝试 $try/4)…"
    code="$(cput -o /tmp/wdp_install.out -w '%{http_code}' \
      -H "X-CSRF-Token: $TOK" \
      "${PARTS[@]}" \
      -X POST "$BASE/api/app/packagemanager/package?package=${NAME}")" && { sent=1; break; }
    echo "  上传中断,3s 后重试…" >&2; sleep 3
  done
  [ "$sent" = 0 ] && { echo "✗ 上传 4 次均失败" >&2; exit 5; }
  echo "安装请求: HTTP $code  $(cat /tmp/wdp_install.out 2>/dev/null)"

  echo "轮询安装状态…"
  for i in $(seq 1 60); do
    sleep 2
    st="$(cput -w '\n%{http_code}' "$BASE/api/app/packagemanager/state")"
    body="$(printf '%s' "$st" | sed '$d')"; hc="$(printf '%s' "$st" | tail -1)"
    if [ "$hc" = "204" ] || [ -z "$body" ]; then echo "安装结束(无进行中任务)"; break; fi
    if printf '%s' "$body" | grep -q '"Success"[[:space:]]*:[[:space:]]*true'; then echo "安装完成 ✓"; break; fi
    if printf '%s' "$body" | grep -q '"Success"[[:space:]]*:[[:space:]]*false' && ! printf '%s' "$body" | grep -qiE 'InProgress|Installing'; then
      echo "✗ 安装失败: $body" >&2; exit 3
    fi
  done

  pfn="$(get_pfn)"
  [ -n "$pfn" ] && echo "已装: $pfn" || echo "⚠ 安装后未查到包名"
  # 顺手开崩溃收集
  if [ -n "$pfn" ]; then
    cput -H "X-CSRF-Token: $(csrf)" -X POST "$BASE/api/debug/dump/usermode/crashcontrol?packageFullName=$(enc_path "$pfn")" >/dev/null 2>&1 \
      && echo "崩溃收集: 已开启"
  fi
  echo "=== 部署完成 ==="
  ;;

diag)
  # 抓 stage.txt(每次加载写 after-load + 诊断行:loads=S/R/C/F js=启用/可执行 scripts=数 nonwhite=..)
  pfn="$(get_pfn)"; [ -z "$pfn" ] && { echo "✗ 未查到已装 Harness 包" >&2; exit 1; }
  enc="$(enc_path "$pfn")"
  got=""
  for rel in 'LocalState\stage.txt' 'stage.txt'; do
    body="$(cput "$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$enc&filename=$(enc_path "$rel")")"
    # WDP 找不到文件会回 JSON 错误({"Reason":...}),真内容含 after-load/loads=
    if printf '%s' "$body" | grep -qiE 'after-load|loads=|url='; then got="$body"; break; fi
  done
  if [ -n "$got" ]; then echo "==== stage.txt ===="; printf '%s\n' "$got"; else
    echo "✗ 没抓到 stage.txt(先在手机上加载一个网页再来;或文件路径不同)。" >&2
    echo "调试:试 ./wdp.sh pull 'LocalState\\stage.txt' -  看返回。" >&2; exit 1
  fi
  ;;

pull)
  REL="${1:-LocalState\\stage.txt}"
  OUT="${2:--}"   # - 表示打印到屏幕
  pfn="$(get_pfn)"
  [ -z "$pfn" ] && { echo "✗ 未查到已装 Harness 包" >&2; exit 1; }
  url="$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$(enc_path "$pfn")&filename=$(enc_path "$REL")"
  if [ "$OUT" = "-" ]; then
    echo "==== $REL ===="
    cput "$url" || { echo "✗ 抓取失败(文件不存在?路径:$REL)" >&2; exit 1; }
    echo
  else
    cput -o "$OUT" "$url" && echo "已存: $OUT ($(wc -c <"$OUT") bytes)" || { echo "✗ 抓取失败" >&2; exit 1; }
  fi
  ;;

dump-on)
  pfn="$(get_pfn)"; [ -z "$pfn" ] && { echo "✗ 未查到包" >&2; exit 1; }
  code="$(cput -o /dev/null -w '%{http_code}' -H "X-CSRF-Token: $(csrf)" \
    -X POST "$BASE/api/debug/dump/usermode/crashcontrol?packageFullName=$(enc_path "$pfn")")"
  echo "开启崩溃收集: HTTP $code  (然后在手机上复现闪退,再跑 ./wdp.sh dump)"
  ;;

dump)
  pfn="$(get_pfn)"; [ -z "$pfn" ] && { echo "✗ 未查到包" >&2; exit 1; }
  enc="$(enc_path "$pfn")"
  list="$(cput "$BASE/api/debug/dump/usermode/dumps?packageFullName=$enc")"
  # 固件差异:DumpFiles 或 CrashDumps
  files="$(printf '%s' "$list" | grep -o '"FileName":"[^"]*"' | sed 's/.*://; s/"//g')"
  [ -z "$files" ] && { echo "设备上暂无本包 dump。先 ./wdp.sh dump-on,在手机复现闪退,再来。"; echo "原始: $list"; exit 0; }
  echo "本包 dump:"; printf '%s\n' "$files" | sed 's/^/  /'
  newest="$(printf '%s\n' "$files" | tail -1)"
  mkdir -p ./crash
  out="./crash/$newest"
  echo "下载最新: $newest"
  cput -o "$out" "$BASE/api/debug/dump/usermode/crashdump?packageFullName=$enc&fileName=$(enc_path "$newest")" \
    && echo "  -> $out ($(wc -c <"$out") bytes)。把它发我,我帮你定位异常码/栈。"
  ;;

help|*)
  sed -n '2,40p' "$0"
  ;;
esac
