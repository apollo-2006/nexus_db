CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17 -O3 -pthread -fPIC -MMD -MP
INCLUDES = -I./include

SRC_DIR = src
OBJ_DIR = obj

SRCS = $(wildcard $(SRC_DIR)/*.cpp)
OBJS = $(patsubst $(SRC_DIR)/%.cpp, $(OBJ_DIR)/%.o, $(SRCS))

TARGET = nexus_db
LIB_TARGET = libnexus.so

all: $(TARGET) $(LIB_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# Compile the shared object library for Python FFI, excluding the main benchmark executable
$(LIB_TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -shared -o $@ $(filter-out $(OBJ_DIR)/main.o, $(OBJS))

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# Engine tests, built with ASan and UBSan. Engine log lines are filtered out.
test: tests/test_db.cpp src/db.cpp
	$(CXX) -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra $(INCLUDES) $^ -o test_db
	./test_db | grep -v '^\[NexusDB\]'

-include $(OBJS:.o=.d)

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(LIB_TARGET) test_db test_db_data bench_db_data dashboard_data

.PHONY: all clean test