TARGET := rte-httpd
SRCS := src/rte-httpd.c

ifneq ($(CROSS_COMPILE),)
CC := $(CROSS_COMPILE)gcc
else
CC ?= cc
endif
CFLAGS ?= -Os -Wall -Wextra -Wno-format-truncation -std=c99
CPPFLAGS ?= -D_DEFAULT_SOURCE
LDFLAGS ?=

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
