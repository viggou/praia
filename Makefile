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

# ── Install / Uninstall ──
# Usage: make install PREFIX=/usr/local
#        make install PREFIX=/usr LIBDIR=/usr/share/praia
#
# Paths:
#   Binary:  $(PREFIX)/bin/praia
#   Grains:  $(LIBDIR)/grains/         (LIBDIR defaults to $(PREFIX)/lib/praia)
#   Sand:    $(LIBDIR)/sand/
#
# LIBDIR is baked into the binary at compile time so grains resolve
# without relative path guessing. DESTDIR is supported for staging.

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib/praia
BINDIR   = $(PREFIX)/bin
SAND_DIR = sand

install: clean
	$(MAKE) CXXFLAGS='$(CXXFLAGS) -DPRAIA_LIBDIR="\"$(LIBDIR)\""'
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(LIBDIR)/grains
	cp -R grains/* $(DESTDIR)$(LIBDIR)/grains/
	install -d $(DESTDIR)$(LIBDIR)/sand
	cp -R $(SAND_DIR)/main.praia $(DESTDIR)$(LIBDIR)/sand/
	cp -R $(SAND_DIR)/grains $(DESTDIR)$(LIBDIR)/sand/
	cp -R $(SAND_DIR)/grain.yaml $(DESTDIR)$(LIBDIR)/sand/
	@printf '#!/bin/sh\nexec "$(BINDIR)/praia" "$(LIBDIR)/sand/main.praia" "$$@"\n' > $(DESTDIR)$(BINDIR)/sand
	chmod 755 $(DESTDIR)$(BINDIR)/sand
	@echo "Installed praia -> $(DESTDIR)$(BINDIR)/praia"
	@echo "Installed sand  -> $(DESTDIR)$(BINDIR)/sand"
	@echo "Installed grains -> $(DESTDIR)$(LIBDIR)/grains/"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/praia
	rm -f $(DESTDIR)$(BINDIR)/sand
	rm -rf $(DESTDIR)$(LIBDIR)
	@echo "Uninstalled praia from $(DESTDIR)$(PREFIX)"

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

.PHONY: all clean install uninstall test test-input
