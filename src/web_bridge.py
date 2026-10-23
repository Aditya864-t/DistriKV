#!/usr/bin/env python3
"""
DistriKV - Web Management Dashboard HTTP/Socket Bridge Server
Bridges web browser REST requests to C++ Berkeley Sockets cluster nodes.
"""

import sys
import os
import json
import socket
import struct
import subprocess
import signal
import time
import re
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

# Configuration
DEFAULT_HTTP_PORT = 8080
CLUSTER_NODES = [
    {"id": 1, "host": "127.0.0.1", "port": 8001, "hb_port": 9001},
    {"id": 2, "host": "127.0.0.1", "port": 8002, "hb_port": 9002},
    {"id": 3, "host": "127.0.0.1", "port": 8003, "hb_port": 9003},
]

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WEB_DIR = os.path.join(ROOT_DIR, "web")
LOGS_DIR = os.path.join(ROOT_DIR, "logs")

def recv_all(sock, n):
    """Receive exactly n bytes from socket"""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)

def send_socket_command(host, port, cmd_str, timeout=2.0):
    """
    Connects to cluster node via native Berkeley Socket,
    sends command, and parses binary framed or text response.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        s.sendall((cmd_str.strip() + "\n").encode("utf-8"))

        # Peek first 4 bytes to check for binary magic 0x444B5631
        peek = s.recv(4, socket.MSG_PEEK)
        if len(peek) >= 4 and struct.unpack("<I", peek)[0] == 0x444B5631:
            hdr_bytes = recv_all(s, 24)
            if not hdr_bytes:
                s.close()
                return {"success": False, "error": "Incomplete header"}

            magic, ver, mtype, flags, term, length, checksum = struct.unpack("<IBBHQII", hdr_bytes)
            payload = ""
            if length > 0:
                p_bytes = recv_all(s, length)
                if p_bytes:
                    payload = p_bytes.decode("utf-8", errors="replace")

            s.close()
            return {"success": True, "data": payload, "type": mtype, "term": term}
        else:
            # Line text fallback
            resp = s.recv(65536).decode("utf-8", errors="replace").strip()
            s.close()
            return {"success": True, "data": resp}
    except Exception as e:
        try:
            s.close()
        except:
            pass
        return {"success": False, "error": str(e)}

def find_current_leader():
    """Finds current primary leader by querying nodes"""
    for node in CLUSTER_NODES:
        res = send_socket_command(node["host"], node["port"], "CLUSTER", timeout=0.8)
        if res.get("success"):
            try:
                info = json.loads(res["data"])
                if info.get("role") == "PRIMARY":
                    return node["host"], node["port"], node["id"]
                leader_endpoint = info.get("leader_endpoint", "")
                if ":" in leader_endpoint:
                    h, p = leader_endpoint.split(":")
                    return h, int(p), info.get("leader_id", -1)
            except:
                pass
    return CLUSTER_NODES[0]["host"], CLUSTER_NODES[0]["port"], 1

class DistriKvBridgeHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB_DIR, **kwargs)

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        if self.path.startswith("/api/"):
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == "/api/cluster":
            self.handle_api_cluster()
        elif url.path == "/api/keys":
            self.handle_api_keys()
        else:
            super().do_GET()

    def do_POST(self):
        url = urlparse(self.path)
        content_len = int(self.headers.get("Content-Length", 0))
        post_body = self.rfile.read(content_len).decode("utf-8") if content_len > 0 else ""
        
        try:
            payload = json.loads(post_body) if post_body else {}
        except:
            payload = {}

        if url.path == "/api/query":
            self.handle_api_query(payload)
        elif url.path == "/api/node/kill":
            self.handle_api_node_kill(payload)
        elif url.path == "/api/node/start":
            self.handle_api_node_start(payload)
        elif url.path == "/api/benchmark":
            self.handle_api_benchmark(payload)
        else:
            self.send_response(404)
            self.end_headers()

    def send_json_response(self, data, status_code=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_api_cluster(self):
        nodes_info = []
        leader_id = -1
        leader_endpoint = ""
        current_term = 0
        total_client_ops = 0

        for node in CLUSTER_NODES:
            res = send_socket_command(node["host"], node["port"], "CLUSTER", timeout=0.8)
            node_data = {
                "id": node["id"],
                "host": node["host"],
                "tcp_port": node["port"],
                "heartbeat_port": node["hb_port"],
                "alive": False,
                "role": "OFFLINE",
                "term": 0,
                "uptime_seconds": 0,
                "total_keys": 0,
                "client_ops": 0,
                "client_put_count": 0,
                "client_get_count": 0,
                "client_del_count": 0,
                "put_count": 0,
                "get_count": 0,
                "del_count": 0,
                "memory_bytes": 0,
                "last_log_index": 0,
                "peers": []
            }

            if res.get("success"):
                try:
                    parsed = json.loads(res["data"])
                    node_data.update(parsed)
                    node_data["alive"] = True
                    total_client_ops += parsed.get("client_ops", 0)
                    if parsed.get("role") == "PRIMARY":
                        leader_id = node["id"]
                        leader_endpoint = f"{node['host']}:{node['port']}"
                        current_term = parsed.get("term", 0)
                except Exception as ex:
                    node_data["error"] = str(ex)
            else:
                node_data["error"] = res.get("error")

            nodes_info.append(node_data)

        if leader_id == -1:
            for nd in nodes_info:
                if nd.get("alive") and nd.get("leader_id", -1) > 0:
                    leader_id = nd["leader_id"]
                    leader_endpoint = nd.get("leader_endpoint", "")
                    current_term = nd.get("term", 0)
                    break

        active_count = sum(1 for n in nodes_info if n.get("alive"))

        self.send_json_response({
            "status": "healthy" if active_count == len(CLUSTER_NODES) else ("degraded" if active_count > 0 else "down"),
            "active_nodes": active_count,
            "total_nodes": len(CLUSTER_NODES),
            "leader_id": leader_id,
            "leader_endpoint": leader_endpoint,
            "current_term": current_term,
            "total_client_ops": total_client_ops,
            "nodes": nodes_info
        })

    def handle_api_keys(self):
        """
        Fetches all keys and values in a single internal batch dump.
        Does NOT trigger individual client GET operations!
        """
        lh, lp, lid = find_current_leader()
        res = send_socket_command(lh, lp, "KEYS DUMP", timeout=2.0)
        keys_list = []
        if res.get("success") and res.get("data"):
            lines = res["data"].split("\n")
            for line in lines:
                if not line:
                    continue
                parts = line.split("\t", 1)
                k = parts[0]
                v = parts[1] if len(parts) > 1 else ""
                keys_list.append({
                    "key": k,
                    "value": v,
                    "size_bytes": len(k) + len(v)
                })

        self.send_json_response({
            "success": True,
            "count": len(keys_list),
            "keys": keys_list
        })

    def handle_api_query(self, payload):
        command = payload.get("command", "").strip()
        if not command:
            self.send_json_response({"success": False, "error": "Empty command"}, 400)
            return

        lh, lp, lid = find_current_leader()
        target_host = payload.get("host", lh)
        target_port = payload.get("port", lp)

        res = send_socket_command(target_host, target_port, command, timeout=2.5)

        if res.get("success") and res.get("data", "").startswith("MOVED "):
            redirect_target = res["data"].split()[1]
            parts = redirect_target.split(":")
            if len(parts) == 2:
                rh, rp = parts[0], int(parts[1])
                res = send_socket_command(rh, rp, command, timeout=2.5)
                res["redirected_to"] = redirect_target

        self.send_json_response(res)

    def handle_api_node_kill(self, payload):
        node_id = payload.get("id")
        if not node_id:
            self.send_json_response({"success": False, "error": "Missing node id"}, 400)
            return

        pid_file = os.path.join(LOGS_DIR, f"node{node_id}.pid")
        if os.path.exists(pid_file):
            try:
                with open(pid_file, "r") as f:
                    pid = int(f.read().strip())
                os.kill(pid, signal.SIGKILL)
                os.remove(pid_file)
            except Exception as e:
                return self.send_json_response({"success": False, "error": str(e)}, 500)
        else:
            target_port = 8000 + int(node_id)
            subprocess.run(f"fuser -k -9 {target_port}/tcp", shell=True, stderr=subprocess.DEVNULL)

        self.send_json_response({
            "success": True,
            "message": f"Node {node_id} terminated. Cluster failure detector will trigger failover."
        })

    def handle_api_node_start(self, payload):
        node_id = int(payload.get("id", 1))
        tcp_port = 8000 + node_id
        hb_port = 9000 + node_id
        peers = []
        for other_id in [1, 2, 3]:
            if other_id != node_id:
                peers.append(f"--peer {other_id},127.0.0.1,{8000 + other_id},{9000 + other_id}")

        peer_args = " ".join(peers)
        cmd = f"./bin/distrikv-node --id {node_id} --host 127.0.0.1 --port {tcp_port} --hb-port {hb_port} --role SECONDARY {peer_args} --data-dir data > logs/node{node_id}.log 2>&1 & echo $! > logs/node{node_id}.pid"

        subprocess.Popen(cmd, shell=True, cwd=ROOT_DIR)
        time.sleep(0.5)

        self.send_json_response({
            "success": True,
            "message": f"Node {node_id} revived on port {tcp_port}. Rejoining cluster..."
        })

    def handle_api_benchmark(self, payload):
        ops = int(payload.get("ops", 500))
        lh, lp, _ = find_current_leader()
        bench_bin = os.path.join(ROOT_DIR, "bin", "distrikv-bench")
        cmd = [bench_bin, "--host", lh, "--port", str(lp), "--threads", "2", "--ops", str(ops // 2)]
        
        try:
            start_t = time.time()
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            elapsed = time.time() - start_t
            output = proc.stdout
            
            throughput = 0.0
            latency = 0.0
            for line in output.split("\n"):
                clean_line = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', line).strip()
                if "Throughput:" in clean_line:
                    parts = clean_line.split("Throughput:")
                    if len(parts) > 1:
                        raw_val = parts[1].replace("ops/sec", "").strip()
                        try:
                            throughput = float(raw_val)
                        except:
                            pass
                elif "Avg Latency:" in clean_line:
                    parts = clean_line.split("Avg Latency:")
                    if len(parts) > 1:
                        raw_val = parts[1].replace("ms/op", "").strip()
                        try:
                            latency = float(raw_val)
                        except:
                            pass

            self.send_json_response({
                "success": True,
                "ops": ops,
                "elapsed_sec": round(elapsed, 3),
                "throughput_ops_sec": throughput,
                "latency_ms": latency,
                "raw_output": re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', output)
            })
        except Exception as e:
            self.send_json_response({"success": False, "error": str(e)}, 500)

def main():
    port = DEFAULT_HTTP_PORT
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except:
            pass

    os.chdir(ROOT_DIR)
    server_address = ("0.0.0.0", port)
    httpd = HTTPServer(server_address, DistriKvBridgeHandler)
    print(f"============================================================")
    print(f" DistriKV Web Bridge & Management Dashboard Running")
    print(f" Web UI:  http://127.0.0.1:{port}")
    print(f" Connected to Cluster Sockets on 8001, 8002, 8003")
    print(f"============================================================")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping Web Bridge Server...")
        httpd.server_close()

if __name__ == "__main__":
    main()
