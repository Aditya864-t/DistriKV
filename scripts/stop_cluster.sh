#!/usr/bin/env bash
# ==============================================================================
# DistriKV - Stop Cluster Script
# Gracefully terminates all cluster nodes and web bridge
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

echo "[Cluster] Stopping all running DistriKV processes..."

for node_id in 1 2 3; do
    if [ -f "logs/node${node_id}.pid" ]; then
        PID=$(cat "logs/node${node_id}.pid")
        if kill -0 "${PID}" 2>/dev/null; then
            echo "  Stopping Node ${node_id} (PID ${PID})..."
            kill -15 "${PID}" 2>/dev/null || kill -9 "${PID}" 2>/dev/null
        fi
        rm -f "logs/node${node_id}.pid"
    fi
done

# Kill any leftover distrikv-node processes
pkill -f "distrikv-node" 2>/dev/null || true
pkill -f "web_bridge.py" 2>/dev/null || true
rm -f logs/web_bridge.pid

echo "[Cluster] All nodes stopped."
