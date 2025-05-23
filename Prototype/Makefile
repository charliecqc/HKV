# Compiler
CXX = g++

# Compiler flags
CXXFLAGS = -Wall -Iinclude -g --std=c++17 -fsanitize=thread -O0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O0
#CXXFLAGS = -Wall -Iinclude --std=c++17 -O3
#CXXFLAGS = -Wall -Iinclude --std=c++17 -g -fsanitize=thread -O3
#CXXFLAGS = -Wall -Iinclude -g --std=c++17 -O0

# Directories
SRC_DIR = src
BUILD_DIR = build
INCLUDE_DIR = include

# Source files
SRCS = $(wildcard $(SRC_DIR)/*.cpp)
# Debug flags
DEBUG_FLAGS = -g
# Object files
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))
# Executable name
TARGET = project
# Default target
all: $(TARGET)
# Linking
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) -o $@ $^

# Compiling
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEBUG_FLAGS) -c -o $@ $<

# Create build directory if it doesn't exist
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Clean target
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Phony targets
.PHONY: all clean
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
