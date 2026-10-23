#!/usr/bin/env bash
# ==============================================================================
# DistriKV - Automated Failover & Replication Verification Suite
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

GREEN='\033[1;32m'
RED='\033[1;31m'
YELLOW='\033[1;33m'
BLUE='\033[1;34m'
NC='\033[0m'

PASSED_COUNT=0
TOTAL_TESTS=0

assert_equal() {
    local test_name="$1"
    local expected="$2"
    local actual="$3"
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    if [[ "$expected" == "$actual" ]]; then
        echo -e "  [${GREEN}PASS${NC}] ${test_name}"
        PASSED_COUNT=$((PASSED_COUNT + 1))
    else
        echo -e "  [${RED}FAIL${NC}] ${test_name} (Expected: '${expected}', Got: '${actual}')"
    fi
}

assert_contains() {
    local test_name="$1"
    local needle="$2"
    local haystack="$3"
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    if [[ "$haystack" == *"$needle"* ]]; then
        echo -e "  [${GREEN}PASS${NC}] ${test_name}"
        PASSED_COUNT=$((PASSED_COUNT + 1))
    else
        echo -e "  [${RED}FAIL${NC}] ${test_name} (Expected to contain: '${needle}', Got: '${haystack}')"
    fi
}

echo -e "${BLUE}==========================================================${NC}"
echo -e "${BLUE} Starting DistriKV Automated Fault-Tolerance Test Suite   ${NC}"
echo -e "${BLUE}==========================================================${NC}"

# Clean data and stop previous instances
bash scripts/stop_cluster.sh > /dev/null 2>&1 || true
rm -f data/*.kv logs/*.log logs/*.pid

# 1. Launch cluster
echo -e "\n${YELLOW}Step 1: Spawning 3-Node Cluster (1 Primary + 2 Secondaries)...${NC}"
bash scripts/run_cluster.sh > /dev/null 2>&1
sleep 2

# 2. Check Primary on 8001
echo -e "\n${YELLOW}Step 2: Testing Primary Node Connectivity & CRUD...${NC}"
RESP=$(./bin/distrikv-cli --host 127.0.0.1 --port 8001 "PUT user:1001 Alice")
assert_equal "PUT user:1001 on Node 1" "OK" "$RESP"

RESP=$(./bin/distrikv-cli --host 127.0.0.1 --port 8001 "PUT user:1002 Bob")
assert_equal "PUT user:1002 on Node 1" "OK" "$RESP"

RESP=$(./bin/distrikv-cli --host 127.0.0.1 --port 8001 "GET user:1001")
assert_equal "GET user:1001 from Node 1" "Alice" "$RESP"

# 3. Check Replication on Secondary Node 2 and Node 3
echo -e "\n${YELLOW}Step 3: Verifying State Replication to Secondaries...${NC}"
sleep 1 # Allow replication transmission

RESP_N2=$(./bin/distrikv-cli --host 127.0.0.1 --port 8002 "GET user:1001")
assert_equal "Replication to Node 2 (user:1001)" "Alice" "$RESP_N2"

RESP_N3=$(./bin/distrikv-cli --host 127.0.0.1 --port 8003 "GET user:1002")
assert_equal "Replication to Node 3 (user:1002)" "Bob" "$RESP_N3"

# 4. Check Secondary Write Redirect
echo -e "\n${YELLOW}Step 4: Verifying Write-to-Secondary Redirection...${NC}"
RESP_REDIR=$(./bin/distrikv-cli --host 127.0.0.1 --port 8002 "PUT user:1003 Charlie")
assert_contains "Auto-redirect on Secondary Write" "OK" "$RESP_REDIR"

# 5. Simulate Primary Node Failure (KILL -9)
echo -e "\n${YELLOW}Step 5: Simulating Crash of Primary Node 1 (SIGKILL)...${NC}"
NODE1_PID=$(cat logs/node1.pid)
echo "  Killing Node 1 PID ${NODE1_PID} with SIGKILL..."
kill -9 "${NODE1_PID}"
rm -f logs/node1.pid

# 6. Wait for leader election (timeout is ~2-3 seconds)
echo -e "\n${YELLOW}Step 6: Waiting for Heartbeat Timeout and Leader Election...${NC}"
echo "  Sleeping 4.0 seconds for followers to detect loss and elect new leader..."
sleep 4.0

# 7. Check Cluster Status from Node 2 and Node 3 to find new Primary
echo -e "\n${YELLOW}Step 7: Identifying New Primary Node...${NC}"
N2_STATUS=$(./bin/distrikv-cli --host 127.0.0.1 --port 8002 "CLUSTER" 2>/dev/null || true)
N3_STATUS=$(./bin/distrikv-cli --host 127.0.0.1 --port 8003 "CLUSTER" 2>/dev/null || true)

NEW_LEADER_PORT=0
if [[ "$N2_STATUS" == *"\"role\": \"PRIMARY\""* ]]; then
    NEW_LEADER_PORT=8002
    echo -e "  ${GREEN}Node 2 won election and became PRIMARY!${NC}"
elif [[ "$N3_STATUS" == *"\"role\": \"PRIMARY\""* ]]; then
    NEW_LEADER_PORT=8003
    echo -e "  ${GREEN}Node 3 won election and became PRIMARY!${NC}"
else
    echo -e "  ${RED}No new leader detected in cluster!${NC}"
fi

assert_contains "Election produced a new PRIMARY leader" "PRIMARY" "$N2_STATUS$N3_STATUS"

# 8. Verify Zero Data Loss on New Primary
echo -e "\n${YELLOW}Step 8: Verifying Zero Data Loss on Surviving Leader...${NC}"
if [[ $NEW_LEADER_PORT -ne 0 ]]; then
    RESP_RESTORED1=$(./bin/distrikv-cli --host 127.0.0.1 --port $NEW_LEADER_PORT "GET user:1001")
    assert_equal "Data persistence after failover (user:1001)" "Alice" "$RESP_RESTORED1"

    RESP_RESTORED2=$(./bin/distrikv-cli --host 127.0.0.1 --port $NEW_LEADER_PORT "GET user:1002")
    assert_equal "Data persistence after failover (user:1002)" "Bob" "$RESP_RESTORED2"

    RESP_RESTORED3=$(./bin/distrikv-cli --host 127.0.0.1 --port $NEW_LEADER_PORT "GET user:1003")
    assert_equal "Data persistence after failover (user:1003)" "Charlie" "$RESP_RESTORED3"

    RESP_NEW_WRITE=$(./bin/distrikv-cli --host 127.0.0.1 --port $NEW_LEADER_PORT "PUT user:1004 David")
    assert_equal "Writing new key user:1004 to new Leader" "OK" "$RESP_NEW_WRITE"
fi

# 9. Revive Node 1 as a Secondary and verify recovery
echo -e "\n${YELLOW}Step 9: Reviving Node 1 as a Follower and Testing Catch-Up...${NC}"
./bin/distrikv-node \
    --id 1 \
    --host 127.0.0.1 \
    --port 8001 \
    --hb-port 9001 \
    --role SECONDARY \
    --peer 2,127.0.0.1,8002,9002 \
    --peer 3,127.0.0.1,8003,9003 \
    --data-dir data > logs/node1_revived.log 2>&1 &
echo $! > logs/node1.pid

sleep 2

RESP_RECOVERED=$(./bin/distrikv-cli --host 127.0.0.1 --port 8001 "GET user:1004" 2>/dev/null || true)
assert_equal "Node 1 recovered and synchronized new data (user:1004)" "David" "$RESP_RECOVERED"

# Clean up
echo -e "\n${YELLOW}Step 10: Cleaning up cluster...${NC}"
bash scripts/stop_cluster.sh > /dev/null 2>&1 || true

# Summary
echo -e "\n${BLUE}==========================================================${NC}"
echo -e " Tests Completed: ${TOTAL_TESTS}"
echo -e " Tests Passed:    ${GREEN}${PASSED_COUNT} / ${TOTAL_TESTS}${NC}"
if [[ $PASSED_COUNT -eq $TOTAL_TESTS ]]; then
    echo -e " ${GREEN}ALL DISTRIBUTED FAULT-TOLERANCE TESTS PASSED!${NC}"
    echo -e "${BLUE}==========================================================${NC}"
    exit 0
else
    echo -e " ${RED}SOME TESTS FAILED!${NC}"
    echo -e "${BLUE}==========================================================${NC}"
    exit 1
fi
