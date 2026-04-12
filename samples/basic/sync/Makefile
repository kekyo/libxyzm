ifeq ($(origin CC),default)
CC := gcc
endif
CPPFLAGS ?= -I$(LIBXYZM_ROOT)/include -I$(LIBXYZM_ROOT)/samples/support -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -Wpedantic -Wformat=2 -Wundef -Wshadow -Wcast-align -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wold-style-definition -Wconversion -Wsign-conversion -O3 -g
LDFLAGS ?=
LDLIBS ?=
PTHREAD_FLAGS ?= -pthread
UNAME_S ?= $(shell uname -s)

LIBXYZM_ROOT ?= ../../..
LIBXYZM ?= $(LIBXYZM_ROOT)/build/libxyzm.a
TARGET := minizm-sync
SOURCES := \
	minizm_sync.c \
	../../support/minizm_posix.c

ifeq ($(UNAME_S),Linux)
LDLIBS += $(shell pkg-config --libs liburing 2>/dev/null)
endif

ifeq ($(suffix $(LIBXYZM)),.so)
	LDFLAGS += -Wl,-rpath,$(abspath $(dir $(LIBXYZM)))
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(LIBXYZM) $(LIBXYZM_ROOT)/include/libxyzm/xyzm.h $(LIBXYZM_ROOT)/samples/support/minizm_posix.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PTHREAD_FLAGS) $(SOURCES) $(LIBXYZM) $(LDFLAGS) -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)
