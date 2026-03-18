# Compiler
CXX = g++
#CXX = clang++

# Configurable flags (override via command line, e.g. make ENABLE_COEFF_ONE=1)
ENABLE_CACHE_STATS ?= 0
ENABLE_THREAD_KEY_CACHE ?= 1
ENABLE_L1_TLS_CACHE ?= 0
ENABLE_PMEM_STATS ?= 0
ENABLE_HOTPATH_DEBUG_LOG ?= 0
ENABLE_DRAM_BLOOM_FULL ?= 0
ENABLE_DRAM_BLOOM_HOT ?= 1
ENABLE_SPLITPATH_STATS ?= 0
ENABLE_L2_SHARD_CACHE ?= 0
ENABLE_SEARCH_STABILITY ?= 0
ENABLE_SGP ?= 1
ENABLE_IMMEDIATE_FLUSH ?= 0
ENABLE_IMMEDIATE_RECLAIM ?= 0
ENABLE_COEFF_ONE ?= 0
ENABLE_LOG_REPLAY ?= 1
ENABLE_DT_ONLY_RECLAIM ?= 1

# Compiler flags
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -fsanitize=thread -O0
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O2 -DENABLE_CACHE_STATS -DENABLE_SEARCH_STABILITY
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O2 -DENABLE_CACHE_STATS
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O2 -DENABLE_SEARCH_STABILITY
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O -DENABLE_PARENT_RELATION=0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -DENABLE_L2_SHARD_CACHE=0
#CXXFLAGS = -Wall -Iinclude -mavx2 --std=c++17 -O3
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O0 -DENABLE_CACHE_STATS=0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -mavx2 -O3 -march=native -flto -DENABLE_CACHE_STATS=0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -flto -DENABLE_CACHE_STATS=0 
#CXXFLAGS = -Wall -Iinclude --std=c++17 -g -O0 -flto -DENABLE_CACHE_STATS=0
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O0 -DENABLE_CACHE_STATS=0 -DENABLE_THREAD_KEY_CACHE=1 -DENABLE_L2_SHARD_CACHE=0 -DENABLE_L1_TLS_CACHE=1 -DENABLE_PMEM_STATS=0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -DENABLE_CACHE_STATS=1
CXXFLAGS_EXTRA ?=
CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -flto=auto -mavx2 -mssse3 -mbmi -mlzcnt -mbmi2 -DENABLE_CACHE_STATS=$(ENABLE_CACHE_STATS) -DENABLE_THREAD_KEY_CACHE=$(ENABLE_THREAD_KEY_CACHE) -DENABLE_L1_TLS_CACHE=$(ENABLE_L1_TLS_CACHE) -DENABLE_PMEM_STATS=$(ENABLE_PMEM_STATS) -DENABLE_HOTPATH_DEBUG_LOG=$(ENABLE_HOTPATH_DEBUG_LOG) -DENABLE_DRAM_BLOOM_FULL=$(ENABLE_DRAM_BLOOM_FULL) -DENABLE_DRAM_BLOOM_HOT=$(ENABLE_DRAM_BLOOM_HOT) -DENABLE_SPLITPATH_STATS=$(ENABLE_SPLITPATH_STATS) -DENABLE_L2_SHARD_CACHE=$(ENABLE_L2_SHARD_CACHE) -DENABLE_SEARCH_STABILITY=$(ENABLE_SEARCH_STABILITY) -DENABLE_SGP=$(ENABLE_SGP) -DENABLE_IMMEDIATE_FLUSH=$(ENABLE_IMMEDIATE_FLUSH) -DENABLE_IMMEDIATE_RECLAIM=$(ENABLE_IMMEDIATE_RECLAIM) -DENABLE_COEFF_ONE=$(ENABLE_COEFF_ONE) -DENABLE_LOG_REPLAY=$(ENABLE_LOG_REPLAY) -DENABLE_DT_ONLY_RECLAIM=$(ENABLE_DT_ONLY_RECLAIM) $(CXXFLAGS_EXTRA)
#CXXFLAGS = -Wall -Iinclude --std=c++17 -g -O0 -DENABLE_CACHE_STAT=0 -DENABLE_TLS_SHADOW_STATS=1
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -flto -DENABLE_CACHE_STATS=0
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O1 -fno-omit-frame-pointer  -DENABLE_CACHE_STATS=0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O2
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -mavx2 -fsanitize=thread -O1
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O0 -DENABLE_SEARCH_STABILITY=1


# Directories
SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = include

SRCS = $(wildcard $(SRC_DIR)/*.cpp)

# Object files
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

# Executable name
TARGET = project

# Default target
all: $(TARGET)

# Linking
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpmemobj -lpthread -lnuma

# Compiling
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $< -lpmemobj -lpthread -lnuma

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Clean target
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Phony targets
.PHONY: all clean
