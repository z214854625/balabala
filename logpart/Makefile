# 定义编译器  
CXX=g++  
  
# 定义编译选项，包括包含目录  
CXXFLAGS=-I./spdlog/include  
  
# 定义源文件  
SRCS=main.cpp HLog.cpp  
  
# 定义目标文件（.o文件），这里使用自动变量，根据源文件生成  
OBJS=$(SRCS:.cpp=.o)  
  
# 定义最终的可执行文件名  
TARGET=log  
  
# 默认目标，即当你只输入make时执行的目标  
all: $(TARGET)  
  
# 链接目标文件生成可执行文件  
$(TARGET): $(OBJS)  
	$(CXX) $(CXXFLAGS) -o $@ $^  
  
# 编译规则，将.cpp文件编译为.o文件  
%.o: %.cpp  
	$(CXX) $(CXXFLAGS) -c -o $@ $<  
  
# 清理编译生成的文件  
clean:  
	rm -f $(OBJS) $(TARGET)  
  
.PHONY: all clean