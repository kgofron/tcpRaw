CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -I./include
LDFLAGS = -pthread

# Directories
SRC_DIR = src
INCLUDE_DIR = include
BUILD_DIR = build
BIN_DIR = bin

# Source files
SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)

# Target executables
TARGET = $(BIN_DIR)/tpx3_parser
TEST_TARGET = $(BIN_DIR)/tcp_raw_test

# Default target
all: $(TARGET) $(TEST_TARGET)

# Create directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Link
$(TARGET): $(BUILD_DIR)/main.o $(BUILD_DIR)/hit_processor.o $(BUILD_DIR)/tcp_server.o $(BUILD_DIR)/timestamp_extension.o $(BUILD_DIR)/tpx3_decoder.o $(BUILD_DIR)/ring_buffer.o | $(BIN_DIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Test program (uses different sources)
$(TEST_TARGET): $(BUILD_DIR)/tcp_raw_test.o $(BUILD_DIR)/tcp_server.o $(BUILD_DIR)/tpx3_decoder.o $(BUILD_DIR)/ring_buffer.o | $(BIN_DIR)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Test program source in test/ directory
$(BUILD_DIR)/tcp_raw_test.o: test/src/tcp_raw_test.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Run
run: $(TARGET)
	$(TARGET)

# Debug build
debug: CXXFLAGS += -g -DDEBUG
debug: clean $(TARGET)

# Install (copy to system path)
install: $(TARGET)
	sudo cp $(TARGET) /usr/local/bin/

.PHONY: all clean run debug install

