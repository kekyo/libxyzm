ifeq ($(origin CC),default)
CC := gcc
endif
ifeq ($(origin CXX),default)
CXX := g++
endif
AR ?= ar
CPPFLAGS ?= -I. -Iinclude -Itests
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wold-style-definition -Wconversion -Wsign-conversion -g -O3
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wundef -Wcast-align -Wconversion -Wsign-conversion -g -O3 -pthread
LDFLAGS ?=
CXX_LDFLAGS ?=
LDLIBS ?=
NODE ?= node
WINE ?= wine
UNAME_S ?= $(shell uname -s)
CARDIO_DIR ?= deps/cardio
CARDIO_CPPFLAGS := -I$(CARDIO_DIR)/include

CC_BASENAME := $(notdir $(CC))

ifeq ($(findstring mingw32,$(CC_BASENAME)),mingw32)
TARGET_OS ?= win32
else
TARGET_OS ?= posix
endif

PUBLIC_HEADERS := \
	include/libxyzm/base.h \
	include/libxyzm/xyzm.h \
	include/libxyzm/sync.h

ASYNC_PUBLIC_HEADERS := \
	$(PUBLIC_HEADERS) \
	include/libxyzm/async.h \
	src/xyzm_async_internal.h

COMMON_HEADERS := $(shell find include src -type f -name '*.h' | sort)

LIBXYZM_COMMON_SOURCES := \
	src/sync/xyzm.c \
	src/xyzm_crc.c

LIBXYZM_API_SOURCES := \
	src/sync/xyzm_xmodem_send_api.c \
	src/sync/xyzm_xmodem_receive_api.c \
	src/sync/xyzm_ymodem_send_api.c \
	src/sync/xyzm_ymodem_receive_api.c \
	src/sync/xyzm_zmodem_send_api.c \
	src/sync/xyzm_zmodem_receive_api.c

LIBXYZM_PROTOCOL_MASTER_SOURCES := \
	src/sync/xyzm_xmodem.c \
	src/sync/xyzm_ymodem.c \
	src/sync/xyzm_zmodem.c

LIBXYZM_ASYNC_SOURCES := \
	src/async/xyzm.cpp \
	src/async/xyzm_xmodem_send_api.cpp \
	src/async/xyzm_xmodem_receive_api.cpp \
	src/async/xyzm_ymodem_send_api.cpp \
	src/async/xyzm_ymodem_receive_api.cpp \
	src/async/xyzm_zmodem_send_api.cpp \
	src/async/xyzm_zmodem_receive_api.cpp \
	src/async/xyzm_xmodem.cpp \
	src/async/xyzm_ymodem.cpp \
	src/async/xyzm_zmodem.cpp

ifeq ($(TARGET_OS),posix)
DEFAULT_BUILD_DIR := build
PIC_CFLAGS := -fPIC
SHARED_RPATH := -Wl,-rpath,'$$ORIGIN'
ifeq ($(UNAME_S),Linux)
LIBURING_CFLAGS := $(shell pkg-config --cflags liburing 2>/dev/null)
LIBURING_LDLIBS := $(shell pkg-config --libs liburing 2>/dev/null)
CARDIO_ASYNC_CPPFLAGS := -DCARDIO_WITH_LINUX_IO_URING=1
endif
PLATFORM_CPPFLAGS := $(LIBURING_CFLAGS)
LIBXYZM_TEST_PLATFORM_LDLIBS := $(LIBURING_LDLIBS)
LIBXYZM_ASYNC_PLATFORM_LDLIBS := $(LIBURING_LDLIBS)
else ifeq ($(TARGET_OS),win32)
DEFAULT_BUILD_DIR := build/$(CC_BASENAME)
PIC_CFLAGS :=
SLICE_MASTER_CFLAGS := -fno-asynchronous-unwind-tables
LDLIBS += -lws2_32
EXE_EXT := .exe
WINEBOOT ?= wineboot
PLATFORM_CPPFLAGS :=
LIBXYZM_TEST_PLATFORM_LDLIBS := -luser32
LIBXYZM_ASYNC_PLATFORM_LDLIBS := -luser32
CXX_LDFLAGS += -static-libstdc++ -static-libgcc
ifeq ($(findstring i686,$(CC_BASENAME)),i686)
SLICE_ROOT_PREFIX := _
else
SLICE_ROOT_PREFIX :=
endif
ifeq ($(findstring i686,$(CC_BASENAME)),i686)
WINE_ARCH ?= win32
else
WINE_ARCH ?= win64
endif
else
$(error Unsupported TARGET_OS '$(TARGET_OS)')
endif

SLICE_ROOT_PREFIX ?=
SLICE_MASTER_CFLAGS ?=

EXE_EXT ?=
BUILD_DIR ?= $(DEFAULT_BUILD_DIR)
ifeq ($(TARGET_OS),win32)
WINE_PREFIX := $(abspath $(BUILD_DIR)/wineprefix)
WINE_PREFIX_STAMP := $(BUILD_DIR)/.wineprefix-$(WINE_ARCH)-ready
WINE_ENV := WINEPREFIX=$(WINE_PREFIX) WINEARCH=$(WINE_ARCH) WINEDEBUG=-all
endif

OBJ_DIR := $(BUILD_DIR)/obj
LIBXYZM_TEST_DIR := $(BUILD_DIR)/tests
WINE_BRIDGE_SCRIPT := tests/common/wine_foreign_bridge.ts
WINE_BRIDGE_PORT_FILE := $(BUILD_DIR)/wine-bridge-port

LIBXYZM_STATIC := $(BUILD_DIR)/libxyzm.a
LIBXYZM_ASYNC_STATIC := $(BUILD_DIR)/libxyzm_async.a
ifeq ($(TARGET_OS),posix)
LIBXYZM_SHARED := $(BUILD_DIR)/libxyzm.so
LIBXYZM_ASYNC_SHARED := $(BUILD_DIR)/libxyzm_async.so
else
LIBXYZM_SHARED := $(BUILD_DIR)/libxyzm.dll
LIBXYZM_IMPLIB := $(BUILD_DIR)/libxyzm.dll.a
LIBXYZM_ASYNC_SHARED := $(BUILD_DIR)/libxyzm_async.dll
LIBXYZM_ASYNC_IMPLIB := $(BUILD_DIR)/libxyzm_async.dll.a
endif

HEADER_SMOKE := $(LIBXYZM_TEST_DIR)/header_smoke$(EXE_EXT)
HEADER_SMOKE_SOURCES := \
	tests/sync/header_smoke.c \
	tests/sync/header_smoke_standalone.c
HEADER_SMOKE_ASYNC := $(LIBXYZM_TEST_DIR)/header_smoke_async$(EXE_EXT)
HEADER_SMOKE_ASYNC_SOURCES := \
	tests/async/header_smoke.cpp
LINK_SCOPE_SYNC_ZMODEM_SEND := $(LIBXYZM_TEST_DIR)/link_scope_sync_zmodem_send$(EXE_EXT)
LIBXYZM_SYNC_TEST_MAIN := $(LIBXYZM_TEST_DIR)/test_sync_main$(EXE_EXT)
LIBXYZM_ASYNC_TEST_MAIN := $(LIBXYZM_TEST_DIR)/test_async_main$(EXE_EXT)
BASIC_SYNC_SAMPLE_TEST := samples/basic/sync/test.sh
TRANSPORT_SYNC_SAMPLE_TEST := samples/transport/sync/test.sh

LIBXYZM_STATIC_BASE_OBJECTS := \
	$(patsubst %.c,$(OBJ_DIR)/%.o,$(LIBXYZM_COMMON_SOURCES)) \
	$(patsubst %.c,$(OBJ_DIR)/%.o,$(LIBXYZM_API_SOURCES))
LIBXYZM_PROTOCOL_MASTER_OBJECTS := \
	$(patsubst %.c,$(OBJ_DIR)/%-master.o,$(LIBXYZM_PROTOCOL_MASTER_SOURCES))
LIBXYZM_SLICE_OBJECTS := \
	$(OBJ_DIR)/src/sync/xyzm_xmodem-send.o \
	$(OBJ_DIR)/src/sync/xyzm_xmodem-receive.o \
	$(OBJ_DIR)/src/sync/xyzm_ymodem-send.o \
	$(OBJ_DIR)/src/sync/xyzm_ymodem-receive.o \
	$(OBJ_DIR)/src/sync/xyzm_zmodem-send.o \
	$(OBJ_DIR)/src/sync/xyzm_zmodem-receive.o
LIBXYZM_STATIC_OBJECTS := $(LIBXYZM_STATIC_BASE_OBJECTS) $(LIBXYZM_SLICE_OBJECTS)
LIBXYZM_SHARED_SOURCES := \
	$(LIBXYZM_COMMON_SOURCES) \
	$(LIBXYZM_API_SOURCES) \
	$(LIBXYZM_PROTOCOL_MASTER_SOURCES)
LIBXYZM_SHARED_PIC_OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%-pic.o,$(LIBXYZM_SHARED_SOURCES))
LIBXYZM_ASYNC_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.async.o,$(LIBXYZM_ASYNC_SOURCES))
LIBXYZM_ASYNC_STATIC_OBJECTS := \
	$(OBJ_DIR)/src/xyzm_crc.o \
	$(LIBXYZM_ASYNC_OBJECTS)
LIBXYZM_ASYNC_PIC_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%-async-pic.o,$(LIBXYZM_ASYNC_SOURCES))
LIBXYZM_ASYNC_SHARED_PIC_OBJECTS := \
	$(OBJ_DIR)/src/xyzm_crc-pic.o \
	$(LIBXYZM_ASYNC_PIC_OBJECTS)

LIBXYZM_SYNC_TEST_SOURCES := \
	tests/sync/main.c \
	tests/sync/core.c \
	tests/sync/unit_xmodem.c \
	tests/sync/unit_ymodem.c \
	tests/sync/unit_zmodem.c \
	tests/sync/transfer_events.c \
	tests/sync/support/test_sync_support.c

ifeq ($(TARGET_OS),posix)
LIBXYZM_SYNC_TEST_SOURCES += \
	tests/sync/integration_xmodem.c \
	tests/sync/integration_ymodem.c \
	tests/sync/integration_zmodem.c
else ifeq ($(TARGET_OS),win32)
LIBXYZM_SYNC_TEST_SOURCES += \
	tests/sync/integration_xmodem.c \
	tests/sync/integration_ymodem.c \
	tests/sync/integration_zmodem.c
endif

LIBXYZM_ASYNC_TEST_SOURCES := \
	tests/async/main.cpp \
	tests/async/core.cpp \
	tests/async/integration_zmodem.cpp \
	tests/async/transfer_events.cpp \
	tests/async/support/test_async_support.cpp

ifeq ($(TARGET_OS),posix)
LIBXYZM_ASYNC_TEST_SOURCES += \
	tests/async/integration_xmodem.cpp
endif

LIBXYZM_SYNC_TEST_OBJECTS := \
	$(patsubst %.c,$(OBJ_DIR)/%.sync-test.o,$(LIBXYZM_SYNC_TEST_SOURCES))
LIBXYZM_ASYNC_TEST_OBJECTS := \
	$(patsubst %.cpp,$(OBJ_DIR)/%.async-test.o,$(LIBXYZM_ASYNC_TEST_SOURCES))

.PHONY: all test clean

ifeq ($(TARGET_OS),posix)
all: $(LIBXYZM_STATIC) $(LIBXYZM_SHARED) $(LIBXYZM_ASYNC_STATIC) $(LIBXYZM_ASYNC_SHARED)
else
all: $(LIBXYZM_STATIC) $(LIBXYZM_SHARED) $(LIBXYZM_ASYNC_STATIC) $(LIBXYZM_ASYNC_SHARED)
endif

ifeq ($(TARGET_OS),posix)
test: $(LIBXYZM_STATIC) $(LIBXYZM_SHARED) $(LIBXYZM_ASYNC_STATIC) $(LIBXYZM_ASYNC_SHARED) $(HEADER_SMOKE) $(HEADER_SMOKE_ASYNC) $(LIBXYZM_SYNC_TEST_MAIN) $(LIBXYZM_ASYNC_TEST_MAIN) $(LINK_SCOPE_SYNC_ZMODEM_SEND)
	./$(HEADER_SMOKE)
	./$(LIBXYZM_SYNC_TEST_MAIN)
	./$(HEADER_SMOKE_ASYNC)
	./$(LIBXYZM_ASYNC_TEST_MAIN)
	$(NODE) tests/check_test_coupling.cjs
	$(NODE) tests/check_build_parallelism.cjs
	$(NODE) tests/check_link_scope.cjs ./$(LINK_SCOPE_SYNC_ZMODEM_SEND) sync-zmodem-send
	$(NODE) tests/check_link_scope.cjs ./$(LIBXYZM_SYNC_TEST_MAIN) sync-tests
	$(NODE) tests/check_link_scope.cjs ./$(LIBXYZM_ASYNC_TEST_MAIN) async-tests
	./$(BASIC_SYNC_SAMPLE_TEST)
	./$(TRANSPORT_SYNC_SAMPLE_TEST)
else
test: $(LIBXYZM_STATIC) $(LIBXYZM_SHARED) $(LIBXYZM_ASYNC_STATIC) $(LIBXYZM_ASYNC_SHARED) $(HEADER_SMOKE) $(HEADER_SMOKE_ASYNC) $(LIBXYZM_SYNC_TEST_MAIN) $(LIBXYZM_ASYNC_TEST_MAIN) $(WINE_PREFIX_STAMP)
	rm -f $(WINE_BRIDGE_PORT_FILE)
	$(NODE) $(WINE_BRIDGE_SCRIPT) $(WINE_BRIDGE_PORT_FILE) & \
	bridge_pid=$$!; \
	trap 'kill $$bridge_pid 2>/dev/null || true; wait $$bridge_pid 2>/dev/null || true' EXIT INT TERM; \
	while [ ! -s $(WINE_BRIDGE_PORT_FILE) ]; do sleep 0.1; done; \
	bridge_port=$$(cat $(WINE_BRIDGE_PORT_FILE)); \
	XYZM_WINE_FOREIGN_BRIDGE_PORT=$$bridge_port XYZM_WINE_TMP_ROOT='Z:/tmp/' $(WINE_ENV) $(WINE) ./$(HEADER_SMOKE); \
	XYZM_WINE_FOREIGN_BRIDGE_PORT=$$bridge_port XYZM_WINE_TMP_ROOT='Z:/tmp/' $(WINE_ENV) $(WINE) ./$(LIBXYZM_SYNC_TEST_MAIN); \
	XYZM_WINE_FOREIGN_BRIDGE_PORT=$$bridge_port XYZM_WINE_TMP_ROOT='Z:/tmp/' $(WINE_ENV) $(WINE) ./$(HEADER_SMOKE_ASYNC); \
	XYZM_WINE_FOREIGN_BRIDGE_PORT=$$bridge_port XYZM_WINE_TMP_ROOT='Z:/tmp/' $(WINE_ENV) $(WINE) ./$(LIBXYZM_ASYNC_TEST_MAIN)
	$(NODE) tests/check_test_coupling.cjs
	$(NODE) tests/check_link_scope.cjs ./$(LIBXYZM_SYNC_TEST_MAIN) sync-tests
	$(NODE) tests/check_link_scope.cjs ./$(LIBXYZM_ASYNC_TEST_MAIN) async-tests
endif

$(LIBXYZM_STATIC): $(LIBXYZM_STATIC_OBJECTS) | $(BUILD_DIR)
	rm -f $@
	$(AR) rcs $@ $(LIBXYZM_STATIC_OBJECTS)

$(LIBXYZM_ASYNC_STATIC): $(LIBXYZM_ASYNC_STATIC_OBJECTS) | $(BUILD_DIR)
	rm -f $@
	$(AR) rcs $@ $(LIBXYZM_ASYNC_STATIC_OBJECTS)

ifeq ($(TARGET_OS),posix)
$(LIBXYZM_SHARED): $(LIBXYZM_SHARED_PIC_OBJECTS) | $(BUILD_DIR)
	$(CC) -shared $(SHARED_RPATH) -Wl,-soname,libxyzm.so $(LDFLAGS) -o $@ $(LIBXYZM_SHARED_PIC_OBJECTS) $(LDLIBS)

$(LIBXYZM_ASYNC_SHARED): $(LIBXYZM_ASYNC_SHARED_PIC_OBJECTS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CXX_LDFLAGS) -shared $(SHARED_RPATH) -Wl,-soname,libxyzm_async.so $(LDFLAGS) -o $@ $(LIBXYZM_ASYNC_SHARED_PIC_OBJECTS) $(LDLIBS) $(LIBXYZM_ASYNC_PLATFORM_LDLIBS)
else
$(LIBXYZM_SHARED): $(LIBXYZM_SHARED_PIC_OBJECTS) | $(BUILD_DIR)
	$(CC) -shared $(LDFLAGS) -Wl,--out-implib,$(LIBXYZM_IMPLIB) -o $@ $(LIBXYZM_SHARED_PIC_OBJECTS) $(LDLIBS)

$(LIBXYZM_ASYNC_SHARED): $(LIBXYZM_ASYNC_SHARED_PIC_OBJECTS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(CXX_LDFLAGS) -shared $(LDFLAGS) -Wl,--out-implib,$(LIBXYZM_ASYNC_IMPLIB) -o $@ $(LIBXYZM_ASYNC_SHARED_PIC_OBJECTS) $(LDLIBS) $(LIBXYZM_ASYNC_PLATFORM_LDLIBS)
endif

$(HEADER_SMOKE): $(HEADER_SMOKE_SOURCES) $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(LIBXYZM_TEST_DIR)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) $(HEADER_SMOKE_SOURCES) -o $@

$(HEADER_SMOKE_ASYNC): $(HEADER_SMOKE_ASYNC_SOURCES) $(ASYNC_PUBLIC_HEADERS) $(COMMON_HEADERS) | $(LIBXYZM_TEST_DIR)
	$(CXX) $(CPPFLAGS) $(CARDIO_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CARDIO_ASYNC_CPPFLAGS) $(CXXFLAGS) $(CXX_LDFLAGS) $(HEADER_SMOKE_ASYNC_SOURCES) -o $@

$(LIBXYZM_SYNC_TEST_MAIN): $(LIBXYZM_SYNC_TEST_OBJECTS) $(LIBXYZM_STATIC) $(PUBLIC_HEADERS) $(COMMON_HEADERS) tests/sync/support/test_sync_support.h | $(LIBXYZM_TEST_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(LIBXYZM_SYNC_TEST_OBJECTS) $(LIBXYZM_STATIC) $(LDLIBS)

$(LIBXYZM_ASYNC_TEST_MAIN): $(LIBXYZM_ASYNC_TEST_OBJECTS) $(LIBXYZM_ASYNC_STATIC) $(ASYNC_PUBLIC_HEADERS) $(COMMON_HEADERS) tests/async/support/test_async_support.h | $(LIBXYZM_TEST_DIR)
	$(CXX) $(CXXFLAGS) $(CXX_LDFLAGS) $(LDFLAGS) -o $@ $(LIBXYZM_ASYNC_TEST_OBJECTS) $(LIBXYZM_ASYNC_STATIC) $(LDLIBS) $(LIBXYZM_TEST_PLATFORM_LDLIBS)

$(LINK_SCOPE_SYNC_ZMODEM_SEND): tests/sync/link_scope_zmodem_send.c $(LIBXYZM_STATIC) $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(LIBXYZM_TEST_DIR)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) tests/sync/link_scope_zmodem_send.c $(LIBXYZM_STATIC) $(LDFLAGS) -o $@ $(LDLIBS)

$(OBJ_DIR)/%.o: %.c $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%-master.o: %.c $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) $(SLICE_MASTER_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(OBJ_DIR)/%-pic.o: %.c $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) $(PIC_CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.sync-test.o: %.c tests/sync/support/test_sync_support.h $(PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.async-test.o: %.cpp tests/async/support/test_async_support.h $(ASYNC_PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CARDIO_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CARDIO_ASYNC_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%.async.o: %.cpp $(ASYNC_PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CARDIO_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CARDIO_ASYNC_CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR)/%-async-pic.o: %.cpp $(ASYNC_PUBLIC_HEADERS) $(COMMON_HEADERS) | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CARDIO_CPPFLAGS) $(PLATFORM_CPPFLAGS) $(CARDIO_ASYNC_CPPFLAGS) $(CXXFLAGS) $(PIC_CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/sync/xyzm_xmodem-send.o: $(OBJ_DIR)/src/sync/xyzm_xmodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_xmodem_send_impl $< -o $@

$(OBJ_DIR)/src/sync/xyzm_xmodem-receive.o: $(OBJ_DIR)/src/sync/xyzm_xmodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_xmodem_receive_impl $< -o $@

$(OBJ_DIR)/src/sync/xyzm_ymodem-send.o: $(OBJ_DIR)/src/sync/xyzm_ymodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_ymodem_send_impl -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_ymodem_send_batch_impl $< -o $@

$(OBJ_DIR)/src/sync/xyzm_ymodem-receive.o: $(OBJ_DIR)/src/sync/xyzm_ymodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_ymodem_receive_impl $< -o $@

$(OBJ_DIR)/src/sync/xyzm_zmodem-send.o: $(OBJ_DIR)/src/sync/xyzm_zmodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_zmodem_send_impl -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_zmodem_send_batch_impl $< -o $@

$(OBJ_DIR)/src/sync/xyzm_zmodem-receive.o: $(OBJ_DIR)/src/sync/xyzm_zmodem-master.o | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -r -Wl,--gc-sections -Wl,-u,$(SLICE_ROOT_PREFIX)xyzm_zmodem_receive_impl $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJ_DIR): | $(BUILD_DIR)
	mkdir -p $(OBJ_DIR)

$(LIBXYZM_TEST_DIR): | $(BUILD_DIR)
	mkdir -p $(LIBXYZM_TEST_DIR)

ifeq ($(TARGET_OS),win32)
$(WINE_PREFIX_STAMP): | $(BUILD_DIR)
	rm -rf $(WINE_PREFIX)
	@if ! $(WINE_ENV) $(WINEBOOT) -u; then \
		echo "Win32 test runtime initialization failed for WINEARCH=$(WINE_ARCH)"; \
		exit 1; \
	fi
	touch $@
endif

clean:
	rm -rf $(BUILD_DIR)
ifneq ($(TARGET_OS),win32)
	$(MAKE) -C samples/basic/sync clean
	$(MAKE) -C samples/transport/sync clean
endif
