CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g -MMD -MP
SRC_DIR = src
BUILD_DIR = build

SOURCES = $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/vm/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS = $(OBJECTS:.o=.d)
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

# Auto-detect OpenSSL (check standard path, then Homebrew)
HAVE_OPENSSL := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lssl -lcrypto -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_OPENSSL),1)
  CXXFLAGS += -DHAVE_OPENSSL
  LDLIBS += -lssl -lcrypto
else
  # Try Homebrew OpenSSL paths (macOS)
  OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
  ifneq ($(OPENSSL_PREFIX),)
    HAVE_OPENSSL := 1
    CXXFLAGS += -DHAVE_OPENSSL -I$(OPENSSL_PREFIX)/include
    LDLIBS += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  endif
endif

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/vm

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

test: $(TARGET) test-input
	./$(TARGET) test

# Integration test for sys.input — needs piped stdin, so runs outside `praia test`.
test-input: $(TARGET)
	@out=$$(printf "Ada\ny\n" | ./$(TARGET) examples/input_demo.praia) && \
	  echo "$$out" | grep -q "Hello, Ada!" && \
	  echo "$$out" | grep -q "Onwards." && \
	  echo "sys.input: ok"
	@out=$$(./$(TARGET) examples/input_demo.praia < /dev/null) && \
	  echo "$$out" | grep -q "No input" && \
	  echo "sys.input EOF: ok"

.PHONY: all clean test test-input
