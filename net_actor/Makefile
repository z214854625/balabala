.PHONY: all clean cleanobj

CXX      = g++
CXXFLAGS = -Wall -g -m64 -fPIC -std=c++20 -fcoroutines -pthread -MMD -MP -include precompiled.h

INCLUDE  = -I./

LDFLAGS  = -L./
LDLIBS   = -ldl -pthread

BIN_PATH = .
TARGET   = net

SOURCES     = $(wildcard *.cc *.cpp)
NODIR_SOURCES = $(notdir $(SOURCES))
OBJECTS     = $(patsubst %.cc,%.o,$(patsubst %.cpp,%.o,$(NODIR_SOURCES)))

OBJECT_PATH    = ../Obj/Net
OBJECT_PREFIX  = $(addprefix $(OBJECT_PATH)/,$(OBJECTS))
DEPS           = $(OBJECT_PREFIX:.o=.d)

all: $(BIN_PATH)/$(TARGET)

$(BIN_PATH)/$(TARGET): $(OBJECT_PREFIX)
	$(CXX) $(CXXFLAGS) $^ $(LDFLAGS) $(LDLIBS) -o $@

$(OBJECT_PATH)/%.o: %.cc | $(OBJECT_PATH)
	$(CXX) $(CXXFLAGS) -c $< $(INCLUDE) -o $@

$(OBJECT_PATH)/%.o: %.cpp | $(OBJECT_PATH)
	$(CXX) $(CXXFLAGS) -c $< $(INCLUDE) -o $@

$(OBJECT_PATH):
	mkdir -p $(OBJECT_PATH)

-include $(DEPS)

cleanobj:
	rm -rf $(OBJECT_PATH)/*.o $(OBJECT_PATH)/*.d

clean: cleanobj
	rm -rf $(BIN_PATH)/$(TARGET)
