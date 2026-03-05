#!/bin/bash
# 在仓库根目录下运行：先启动 Yate，再启动 iot-gateway（用于本地开发）
# 用法：从仓库根目录执行  ./scripts/run-iot-dev.sh

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 启动前设置引导 API Key（iot-gateway 将接受该 Key 为 admin 角色）
export API_KEY=admin123

# 0) 若已有进程在跑，先结束
kill_yate() {
    local pids
    pids=$(pgrep -f "yate -c" 2>/dev/null) || true
    if [ -n "$pids" ]; then
        echo "检测到已运行的 Yate 进程: $pids，正在结束..."
        pkill -f "yate -c" 2>/dev/null || true
        sleep 1
    fi
}
kill_gateway() {
    local pids
    pids=$(pgrep -f "yate-iot-gateway" 2>/dev/null) || true
    if [ -n "$pids" ]; then
        echo "检测到已运行的 iot-gateway 进程: $pids，正在结束..."
        pkill -f "yate-iot-gateway" 2>/dev/null || true
        sleep 1
    fi
}
kill_gateway
kill_yate
echo ""

# 1) 检查并启动 Yate
if [ ! -x "./yate" ]; then
    echo "未找到可执行文件 ./yate，请先编译 Yate："
    echo "  make"
    exit 1
fi

echo "启动 Yate（conf.d=$ROOT/conf.d, modules=$ROOT/modules）..."
LD_LIBRARY_PATH="$ROOT:$ROOT/modules/server:$LD_LIBRARY_PATH" \
    ./yate -c "$ROOT/conf.d" -m "$ROOT/modules" -e "$ROOT/share" &
YATE_PID=$!
echo "Yate PID: $YATE_PID"

# 等待 extmodule 在 5040 监听
sleep 2
if ! command -v ss &>/dev/null; then
    (command -v netstat &>/dev/null && netstat -tlnp 2>/dev/null | grep -q 5040) || true
else
    ss -tlnp 2>/dev/null | grep -q 5040 || true
fi

# 2) 检查并启动 iot-gateway
GWBIN="$ROOT/iot-gateway/yate-iot-gateway"
if [ ! -x "$GWBIN" ]; then
    echo "未找到 $GWBIN，正在编译..."
    (cd "$ROOT/iot-gateway" && go build -o yate-iot-gateway ./cmd/yate-iot-gateway)
fi

echo "启动 iot-gateway（--yate=127.0.0.1:5040 --http=:8088 --mqtt=:1883 --coap=:5683）..."
cleanup() { kill $YATE_PID 2>/dev/null || true; exit 0; }
trap cleanup INT TERM
cd "$ROOT/iot-gateway"
echo "  Yate:        PID $YATE_PID（extmodule :5040）"
echo "  iot-gateway: HTTP :8088, MQTT :1883, CoAP :5683（Ctrl+C 结束两者）"
echo ""
"$GWBIN" --yate=127.0.0.1:5040 --http=:8088 --mqtt=:1883 --coap=:5683
cleanup
