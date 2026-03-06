# Compiler
CXX = g++
#CXX = clang++

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
CXXFLAGS = -Wall -Iinclude --std=c++17 -O3 -flto=auto -mavx2 -mssse3 -mbmi -mlzcnt -mbmi2 -DENABLE_CACHE_STATS=0 -DENABLE_THREAD_KEY_CACHE=1 -DENABLE_L1_TLS_CACHE=0 -DENABLE_PMEM_STATS=0 -DENABLE_HOTPATH_DEBUG_LOG=0 -DENABLE_DRAM_BLOOM_FULL=0 -DENABLE_DRAM_BLOOM_HOT=1 -DENABLE_SPLITPATH_STATS=1 -DENABLE_L2_SHARD_CACHE=0 -DENABLE_SEARCH_STABILITY=0 -DENABLE_SGP=1
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
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpmemobj -lpthread

# Compiling
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $< -lpmemobj -lpthread

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Clean target
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Phony targets
.PHONY: all clean
