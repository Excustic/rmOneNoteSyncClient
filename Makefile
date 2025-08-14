# Makefile for reMarkable Sync Client
# 
# Usage:
#   source /opt/codex/ferrari/4.3.98/environment-setup-cortexa53-crypto-remarkable-linux
#   make all

# The cross-compiler will be set by the environment script
CC ?= $(CC)

BUILD_DIR = build

# Source files
WATCHER_SRCS = watcher.c cache_io.c metadata_parser.c
HTTPCLIENT_SRCS = httpclient.c cache_io.c metadata_parser.c http_simple.c
DEBUG_SRCS = cache_debug.c

# Output binaries
WATCHER_BIN = $(BUILD_DIR)/watcher
HTTPCLIENT_BIN = $(BUILD_DIR)/httpclient
DEBUG_BIN = $(BUILD_DIR)/cache_debug

# Build flags
CFLAGS = -Wall -O2 -g
LDFLAGS = 

# Default target
all: $(BUILD_DIR) $(WATCHER_BIN) $(HTTPCLIENT_BIN) $(DEBUG_BIN)
	@echo "===================================="
	@echo "Build complete!"
	@echo "Binaries created:"
	@echo "  - $(WATCHER_BIN)"
	@echo "  - $(HTTPCLIENT_BIN)"
	@echo "  - $(DEBUG_BIN)"
	@echo "===================================="

# Build watcher
$(WATCHER_BIN): $(WATCHER_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WATCHER_SRCS)
	@echo "Built: $@"

# Build HTTP client
$(HTTPCLIENT_BIN): $(HTTPCLIENT_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(HTTPCLIENT_SRCS)
	@echo "Built: $@"

# Build debug tool
$(DEBUG_BIN): $(DEBUG_SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DEBUG_SRCS)
	@echo "Built: $@"

# Clean build artifacts
clean:
	rm -f $(WATCHER_BIN) $(HTTPCLIENT_BIN) $(DEBUG_BIN)
	@echo "Cleaned build artifacts"

# Show help
help:
	@echo "reMarkable Sync Client Build System"
	@echo ""
	@echo "Prerequisites:"
	@echo "  1. Source the cross-compile environment:"
	@echo "     source /opt/codex/ferrari/4.3.98/environment-setup-cortexa53-crypto-remarkable-linux"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build all binaries (default)"
	@echo "  clean     - Remove built binaries"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Individual targets:"
	@echo "  watcher   - Build only the watcher"
	@echo "  httpclient - Build only the HTTP client"
	@echo "  cache_debug - Build only the debug tool"

.PHONY: all clean help