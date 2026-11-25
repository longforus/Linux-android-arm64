#!/bin/bash
#
# wxshadow R^X shadow 补丁一键回归脚本
#
# 验证链路：服务健康 → 能力查询 → patch → 受管页 → 读隐藏(GUP原文/driver直读补丁)
#          → fork → mprotect(mmu_notifier 拆页) → dmesg 无 bad page → exit
#          → 运行时开关 enable 0/1
#
# 用法：
#   ./wxshadow_regression.sh [adb-serial]
#
# 依赖：aarch64-linux-android*-clang（NDK，用于编译测试二进制）、adb、curl、python3、su。

set -u

SERIAL="${1:-}"
ADB="adb"
[ -n "$SERIAL" ] && ADB="adb -s $SERIAL"

RPC="http://127.0.0.1:9494/api/rpc"
RC="com.android.xsettings/com.longforus.xsettings.XstCommandReceiver"
TEST_BIN="/data/local/tmp/wxshadow_test"
TEST_LOG="/data/local/tmp/wxshadow_test.log"
NOP_HEX="1F2003D5"          # NOP
ORIG_HEX="400580d2"         # 原文 mov x0,#42 的前 4 字节
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_SRC="$SCRIPT_DIR/wxshadow_test.c"

PASS=0
FAIL=0
ok()  { echo "  ✅ $1"; PASS=$((PASS+1)); }
bad() { echo "  ❌ $1"; FAIL=$((FAIL+1)); }

rpc() { curl -s -m 5 -X POST "$RPC" -d "$1"; }

echo "======================================================"
echo " wxshadow R^X shadow 补丁回归"
echo "======================================================"

# ---- 0. 编译并推送测试二进制 ----
echo "=== [0] 准备测试二进制 ==="
if ! $ADB shell "ls $TEST_BIN" >/dev/null 2>&1; then
    CLANG=$(ls /home/void/Library/Android/Sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android*-clang 2>/dev/null | head -1)
    if [ -z "$CLANG" ]; then
        bad "未找到 NDK clang，无法编译测试二进制"
        exit 1
    fi
    "$CLANG" -static -O2 -o /tmp/wxshadow_test "$TEST_SRC" || { bad "编译失败"; exit 1; }
    $ADB push /tmp/wxshadow_test "$TEST_BIN" >/dev/null || { bad "push 失败"; exit 1; }
    $ADB shell "chmod 755 $TEST_BIN"
fi
ok "测试二进制就绪"

# ---- 1. 服务健康 ----
echo "=== [1] 服务健康 ==="
$ADB forward tcp:9494 tcp:9494 >/dev/null 2>&1
HEALTH=$(curl -s -m 5 "$RPC/health" 2>/dev/null)
if ! echo "$HEALTH" | grep -q '"ok":true'; then
    echo "  服务未就绪，尝试 START_HTTP 拉起..."
    $ADB shell am broadcast -a com.longforus.xsettings.action.START_HTTP -n "$RC" --include-stopped-packages >/dev/null 2>&1
    sleep 4
    $ADB forward tcp:9494 tcp:9494 >/dev/null 2>&1
    HEALTH=$(curl -s -m 5 "$RPC/health" 2>/dev/null)
fi
echo "$HEALTH" | grep -q '"ok":true' && ok "health ok" || { bad "health: $HEALTH"; exit 1; }

# ---- 2. 能力查询 ----
echo "=== [2] wxshadow.status ==="
ST=$(rpc '{"operation":"wxshadow.status"}')
echo "$ST" | grep -q '"cap_patch":true' && ok "cap_patch=true" || bad "status: $ST"
echo "$ST" | grep -q '"cap_read_hide":true' && ok "cap_read_hide=true" || bad "status: $ST"

# ---- 3. 启动测试进程 ----
echo "=== [3] 启动测试进程 ==="
$ADB shell "rm -f $TEST_LOG; nohup $TEST_BIN > $TEST_LOG 2>&1 &"
sleep 1
LINE=$($ADB shell "cat $TEST_LOG" | grep 'PID=')
PID=$(echo "$LINE" | sed -n 's/.*PID=\([0-9][0-9]*\).*/\1/p')
ADDR=$(echo "$LINE" | sed -n 's/.*CODE=\(0x[0-9a-fA-F]*\).*/\1/p')
[ -n "$PID" ] && [ -n "$ADDR" ] && ok "PID=$PID CODE=$ADDR" || { bad "无法获取测试进程信息: $LINE"; exit 1; }
SKIP=$((16#${ADDR#0x} / 4096))

# ---- 4. patch ----
echo "=== [4] patch (NOP) ==="
rpc "{\"operation\":\"target.select\",\"params\":{\"pid\":$PID}}" >/dev/null
R=$(rpc "{\"operation\":\"wxshadow.patch\",\"params\":{\"address\":\"$ADDR\",\"data_hex\":\"$NOP_HEX\"}}")
echo "$R" | grep -q '"status":0' && ok "patch status=0" || bad "patch: $R"

# ---- 5. 受管页 ----
echo "=== [5] wxshadow.list ==="
L=$(rpc '{"operation":"wxshadow.list"}')
echo "$L" | grep -q '"page_count":1' && ok "list 1 页" || bad "list: $L"

# ---- 6. 读隐藏 ----
echo "=== [6] 读隐藏 ==="
# GUP (/proc/pid/mem) 应读到原文；driver 直读应看到补丁
GUP=$($ADB shell "su -c 'dd if=/proc/$PID/mem bs=4096 skip=$SKIP count=1 2>/dev/null | head -c 4 | xxd -p'")
DIRECT=$(rpc "{\"operation\":\"memory.read\",\"params\":{\"address\":\"$ADDR\",\"size\":4}}" | python3 -c 'import sys,json; print(json.load(sys.stdin)["data"]["data_hex"][:8].lower())' 2>/dev/null)
echo "$GUP" | grep -qi "$ORIG_HEX" && ok "GUP 读到原文 ($GUP)" || bad "GUP 读到 [$GUP]，期望原文 $ORIG_HEX"
echo "$DIRECT" | grep -qi "$(echo $NOP_HEX | tr 'A-F' 'a-f')" && ok "driver 直读看到补丁 ($DIRECT)" || bad "driver 直读 [$DIRECT]，期望补丁"

# ---- 7. fork ----
echo "=== [7] fork ==="
$ADB shell "su -c 'kill -USR1 $PID'"
sleep 1
$ADB shell "cat $TEST_LOG" | grep -q "fork+reap" && ok "fork+reap" || bad "fork 未完成"

# ---- 8. mprotect (mmu_notifier 拆页) ----
echo "=== [8] mprotect ==="
$ADB shell "su -c 'kill -USR2 $PID'"
sleep 1
$ADB shell "cat $TEST_LOG" | grep -q "mprotect r-x done" && ok "mprotect 完成" || bad "mprotect 未完成"
L=$(rpc '{"operation":"wxshadow.list"}')
echo "$L" | grep -q '"page_count":0' && ok "mprotect 后 list 空（mmu_notifier 拆页）" || bad "mprotect 后 list: $L"

# ---- 9. dmesg 无 bad page ----
echo "=== [9] dmesg 无 Bad page/rss/panic ==="
BADLOG=$($ADB shell "su -c 'dmesg | grep -iE \"bad page|bad rss|BUG:|Kernel panic\"'")
[ -z "$BADLOG" ] && ok "无 bad page/rss/panic" || bad "发现: $BADLOG"

# ---- 10. exit ----
echo "=== [10] exit ==="
$ADB shell "su -c 'kill -TERM $PID'"
sleep 1
$ADB shell "pidof wxshadow_test" >/dev/null 2>&1 && bad "进程仍在" || ok "进程已退出"

# ---- 11. 运行时开关 ----
echo "=== [11] 运行时开关 enable 0/1 ==="
R=$(rpc '{"operation":"wxshadow.enable","params":{"enabled":0}}')
echo "$R" | grep -q '"enabled":false' && ok "enable 0" || bad "enable 0: $R"
R=$(rpc '{"operation":"wxshadow.enable","params":{"enabled":1}}')
echo "$R" | grep -q '"enabled":true' && ok "enable 1" || bad "enable 1: $R"

echo
echo "======================================================"
echo " 结果: $PASS 通过, $FAIL 失败"
if [ "$FAIL" -eq 0 ]; then echo " 🎉 全部通过"; else echo " ⚠️ 有失败项"; fi
echo "======================================================"
exit "$FAIL"
