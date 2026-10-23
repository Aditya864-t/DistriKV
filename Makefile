CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread -Isrc
LDFLAGS = -pthread

BIN_DIR = bin
OBJ_DIR = obj
SRC_DIR = src

COMMON_SRCS = $(SRC_DIR)/socket_util.cpp $(SRC_DIR)/protocol.cpp
NODE_SRCS = $(COMMON_SRCS) $(SRC_DIR)/storage.cpp $(SRC_DIR)/cluster_node.cpp $(SRC_DIR)/node_main.cpp
CLI_SRCS = $(COMMON_SRCS) $(SRC_DIR)/client.cpp
BENCH_SRCS = $(COMMON_SRCS) $(SRC_DIR)/benchmark.cpp

NODE_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(NODE_SRCS))
CLI_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(CLI_SRCS))
BENCH_OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(BENCH_SRCS))

TARGET_NODE = $(BIN_DIR)/distrikv-node
TARGET_CLI = $(BIN_DIR)/distrikv-cli
TARGET_BENCH = $(BIN_DIR)/distrikv-bench

.PHONY: all clean test dirs

all: dirs $(TARGET_NODE) $(TARGET_CLI) $(TARGET_BENCH)
	@echo "Build complete. Executables available in $(BIN_DIR)/"

dirs:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR) logs data

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET_NODE): $(NODE_OBJS)
	$(CXX) $(NODE_OBJS) -o $@ $(LDFLAGS)
	@echo "Built $@"

$(TARGET_CLI): $(CLI_OBJS)
	$(CXX) $(CLI_OBJS) -o $@ $(LDFLAGS)
	@echo "Built $@"

$(TARGET_BENCH): $(BENCH_OBJS)
	$(CXX) $(BENCH_OBJS) -o $@ $(LDFLAGS)
	@echo "Built $@"

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) dump_node_*.kv data/*.kv logs/*.log

test: all
	@bash scripts/test_failover.sh
