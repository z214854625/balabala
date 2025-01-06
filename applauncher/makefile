# 指定编译器
CXX := g++

# 指定源文件与输出目录
SRC_DIR := src
BUILD_DIR := build

# 查找所有 .cc 和 .cpp 文件
SOURCES := $(wildcard $(SRC_DIR)/*.cc) $(wildcard $(SRC_DIR)/*.cpp)

# 生成相应的对象文件
OBJECTS := $(patsubst $(SRC_DIR)/%.cc, $(BUILD_DIR)/%.o, $(SOURCES))
OBJECTS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(OBJECTS))

# 可执行文件名称
TARGET := my_program

# 编译选项
CXXFLAGS := -Wall -Wextra -std=c++17  # 添加其他你需要的编译选项

# 默认目标
all: $(BUILD_DIR) $(TARGET)

# 创建输出目录
$(BUILD_DIR):
    mkdir -p $(BUILD_DIR)

# 链接目标
$(TARGET): $(OBJECTS)
    $(CXX) -o $@ $^

# 编译源文件为对象文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cc
    $(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
    $(CXX) $(CXXFLAGS) -c $< -o $@

# 清理编译生成的文件
clean:
    rm -rf $(BUILD_DIR)/*
    rm -f $(TARGET)

.PHONY: all clean