.PHONY: all clean

G = g++
CFLAGS = -Wall -g -m64 -fPIC -shared -std=c++20 -include precompiled.h

INCLUDE = -I./

LD_PATH = -L./
LD_LIBS = -ldl -lpthread

BIN_PATH = .
TARGET = net

SOURCES = $(wildcard *.c *.cpp *.cc)
NODIER_SOURCES = $(notdir $(SOURCES))
_OBJECTS = $(patsubst %.cpp, %.o, $(NODIER_SOURCES))
OBJECTS = $(patsubst %.cc, %.o, $(_OBJECTS))

OBJECT_PATH = ../Obj/Net
OBJECT_PREFIX = $(addprefix $(OBJECT_PATH)/, $(OBJECTS))

all: $(BIN_PATH)/$(TARGET)

$(BIN_PATH)/$(TARGET): $(OBJECT_PREFIX)
	$(G) $(CFLAGS) $^ $(INCLUDE) $(LD_PATH) $(LD_LIBS) -o $@

$(OBJECT_PATH)/%.o: %.cc
	mkdir -p $(OBJECT_PATH)
	$(G) $(CFLAGS) -c $< $(INCLUDE) -o $@

$(OBJECT_PATH)/%.o: %.cpp
	mkdir -p $(OBJECT_PATH)
	$(G) $(CFLAGS) -c $< $(INCLUDE) -o $@

cleanobj:
	rm -rf $(OBJECT_PATH)/*.o

clean: cleanobj
	rm -rf $(BIN_PATH)/$(TARGET)
