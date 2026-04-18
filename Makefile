CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g
SRC_DIR = src
BUILD_DIR = build

SOURCES = $(wildcard $(SRC_DIR)/*.cpp)
OBJECTS = $(SOURCES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
TARGET = praia

all: $(TARGET)

# Auto-detect readline/libedit support
HAVE_READLINE := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lreadline -o /dev/null 2>/dev/null && echo 1)
HAVE_EDIT     := $(shell echo 'int main(){}' | $(CXX) -x c++ - -ledit -o /dev/null 2>/dev/null && echo 1)

ifeq ($(HAVE_READLINE),1)
  CXXFLAGS += -DHAVE_READLINE
  LDLIBS = -lreadline
else ifeq ($(HAVE_EDIT),1)
  CXXFLAGS += -DHAVE_READLINE
  LDLIBS = -ledit
else
  LDLIBS =
endif

# Auto-detect SQLite
HAVE_SQLITE := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lsqlite3 -o /dev/null 2>/dev/null && echo 1)

ifeq ($(HAVE_SQLITE),1)
  CXXFLAGS += -DHAVE_SQLITE
  LDLIBS += -lsqlite3
endif

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

HEADERS = $(wildcard $(SRC_DIR)/*.h)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

test: $(TARGET)
	./$(TARGET) test

.PHONY: all clean test
