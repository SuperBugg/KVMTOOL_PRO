CXX ?= g++

CXXFLAGS ?= -O2 -Wall -Wextra -fPIC
CPPFLAGS += -Iinclude -Isrc
LDLIBS += -lrknnrt

TARGET := librknn_backend.so
SRC := src/rknn_backend.cpp
OBJ := $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) -shared -o $@ $^ $(LDLIBS)

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
