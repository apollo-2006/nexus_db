CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -pthread -fPIC
INCLUDES = -I./include

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = nexus_db
LIB_TARGET = libnexus.so

all: $(OBJ_DIR) $(TARGET) $(LIB_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile the shared object library for Python FFI, excluding the main benchmark executable
$(LIB_TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $(filter-out $(OBJ_DIR)/main.o, $(OBJS))

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(LIB_TARGET) test_db_data dashboard_data

.PHONY: all clean