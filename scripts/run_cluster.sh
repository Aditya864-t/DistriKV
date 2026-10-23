#!/usr/bin/env bash
# ==============================================================================
# DistriKV - 3-Node Cluster Launch Script
# Starts 1 Primary (Port 8001) + 2 Secondaries (Ports 8002, 8003) + Web Dashboard
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

mkdir -p logs data bin

echo "=========================================================="
echo " Starting DistriKV 3-Node Cluster..."
echo "=========================================================="

# Stop any currently running nodes first
bash "${SCRIPT_DIR}/stop_cluster.sh" > /dev/null 2>&1 || true

# Node 1: Primary (TCP 8001, UDP HB 9001)
echo "[Cluster] Spawning Node 1 (Initial PRIMARY) on TCP 8001, UDP 9001..."
nohup ./bin/distrikv-node \
    --id 1 \
    --host 127.0.0.1 \
    --port 8001 \
    --hb-port 9001 \
    --role PRIMARY \
    --peer 2,127.0.0.1,8002,9002 \
    --peer 3,127.0.0.1,8003,9003 \
    --data-dir data > logs/node1.log 2>&1 &
echo $! > logs/node1.pid

sleep 0.5

# Node 2: Secondary (TCP 8002, UDP HB 9002)
echo "[Cluster] Spawning Node 2 (SECONDARY) on TCP 8002, UDP 9002..."
nohup ./bin/distrikv-node \
    --id 2 \
    --host 127.0.0.1 \
    --port 8002 \
    --hb-port 9002 \
    --role SECONDARY \
    --peer 1,127.0.0.1,8001,9001 \
    --peer 3,127.0.0.1,8003,9003 \
    --data-dir data > logs/node2.log 2>&1 &
echo $! > logs/node2.pid

# Node 3: Secondary (TCP 8003, UDP HB 9003)
echo "[Cluster] Spawning Node 3 (SECONDARY) on TCP 8003, UDP 9003..."
nohup ./bin/distrikv-node \
    --id 3 \
    --host 127.0.0.1 \
    --port 8003 \
    --hb-port 9003 \
    --role SECONDARY \
    --peer 1,127.0.0.1,8001,9001 \
    --peer 2,127.0.0.1,8002,9002 \
    --data-dir data > logs/node3.log 2>&1 &
echo $! > logs/node3.pid

sleep 1

# Launch Web Bridge Server on port 8080
echo "[Web] Spawning Web Dashboard & Socket Bridge on port 8080..."
nohup python3 src/web_bridge.py 8080 > logs/web_bridge.log 2>&1 &
echo $! > logs/web_bridge.pid

echo "=========================================================="
echo " Cluster & Web Dashboard successfully started!"
echo "   Node 1 PID: $(cat logs/node1.pid) [PRIMARY on 8001]"
echo "   Node 2 PID: $(cat logs/node2.pid) [SECONDARY on 8002]"
echo "   Node 3 PID: $(cat logs/node3.pid) [SECONDARY on 8003]"
echo "   Web Dashboard: http://127.0.0.1:8080 (PID $(cat logs/web_bridge.pid))"
echo "   Logs directory: logs/"
echo "=========================================================="

if [[ "$1" == "--foreground" || "$1" == "--daemon" ]]; then
    echo "[Cluster] Running supervisor loop. Press Ctrl+C to terminate."
    trap 'bash "${SCRIPT_DIR}/stop_cluster.sh"; exit 0' SIGINT SIGTERM
    wait
fi
