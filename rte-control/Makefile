# SPDX-License-Identifier: GPL-2.0-or-later

CC ?= cc
AR ?= ar
PKG_CONFIG ?= pkg-config
PYTHON ?= python3
NODE ?= node
PREFIX ?= /usr
DESTDIR ?=
BUILD_DIR ?= build
WITH_HTTP ?= 0
MONGOOSE_CFLAGS ?=
MONGOOSE_LIBS ?= -lmongoose
MONGOOSE_SOURCE ?=
MONGOOSE_LIMIT_CFLAGS = -DMG_MAX_RECV_SIZE=24576 -DMG_ENABLE_DIRLIST=0

CPPFLAGS += -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror -Wformat=2 -Wshadow
LDLIBS_CORE = -lm
IIO_CFLAGS ?= $(shell $(PKG_CONFIG) --cflags libiio 2>/dev/null)
IIO_LIBS ?= $(shell $(PKG_CONFIG) --libs libiio 2>/dev/null)

CORE_OBJECTS = $(BUILD_DIR)/model.o $(BUILD_DIR)/register_io.o $(BUILD_DIR)/mmio.o
CORE_LIBRARY = $(BUILD_DIR)/librtecontrol.a
REGTOOL = $(BUILD_DIR)/rte-regtool
HTTPD = $(BUILD_DIR)/rte-httpd
HTTP_OBJECTS = $(BUILD_DIR)/ad936x.o $(BUILD_DIR)/controller.o
TEST_MODEL = $(BUILD_DIR)/test_model
TEST_REGISTER_IO = $(BUILD_DIR)/test_register_io
TEST_CONTROLLER = $(BUILD_DIR)/test_controller
TEST_HTTP_JSON = $(BUILD_DIR)/test_http_json

ifneq ($(strip $(MONGOOSE_SOURCE)),)
MONGOOSE_DEPS = $(BUILD_DIR)/mongoose.o
MONGOOSE_LINK = $(BUILD_DIR)/mongoose.o
else
MONGOOSE_DEPS =
MONGOOSE_LINK = $(MONGOOSE_LIBS)
endif

.PHONY: all clean test test-http test-ui httpd install install-http

all: $(REGTOOL)

ifeq ($(WITH_HTTP),1)
all: $(HTTPD)
install: install-http
endif

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/model.o: src/model.c include/rte/model.h include/rte/abi.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/register_io.o: src/register_io.c include/rte/register_io.h include/rte/model.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mmio.o: src/mmio.c include/rte/mmio.h include/rte/register_io.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

$(CORE_LIBRARY): $(CORE_OBJECTS)
	$(AR) rcs $@ $^

$(REGTOOL): src/regtool.c $(CORE_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/regtool.c $(CORE_LIBRARY) $(LDLIBS_CORE)

$(TEST_MODEL): tests/test_model.c $(CORE_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_model.c $(CORE_LIBRARY) $(LDLIBS_CORE)

$(TEST_REGISTER_IO): tests/test_register_io.c $(CORE_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_register_io.c $(CORE_LIBRARY) $(LDLIBS_CORE)

$(TEST_CONTROLLER): tests/test_controller.c $(BUILD_DIR)/controller.o $(CORE_LIBRARY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_controller.c \
		$(BUILD_DIR)/controller.o $(CORE_LIBRARY) -lpthread $(LDLIBS_CORE)

test: $(TEST_MODEL) $(TEST_REGISTER_IO) $(TEST_CONTROLLER)
	./$(TEST_MODEL)
	./$(TEST_REGISTER_IO)
	./$(TEST_CONTROLLER)

$(TEST_HTTP_JSON): tests/test_http_json.c src/httpd.c $(CORE_LIBRARY) $(HTTP_OBJECTS) $(MONGOOSE_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IIO_CFLAGS) $(MONGOOSE_CFLAGS) $(LDFLAGS) \
		-o $@ tests/test_http_json.c $(HTTP_OBJECTS) $(CORE_LIBRARY) \
		$(MONGOOSE_LINK) $(IIO_LIBS) -lpthread $(LDLIBS_CORE)

test-http: $(TEST_HTTP_JSON)
	./$(TEST_HTTP_JSON)

test-ui:
	$(NODE) --check www/app.js
	$(PYTHON) tests/test_ui.py

install: all
	install -D -m 0644 $(CORE_LIBRARY) $(DESTDIR)$(PREFIX)/lib/librtecontrol.a
	install -D -m 0755 $(REGTOOL) $(DESTDIR)$(PREFIX)/bin/rte-regtool
	install -D -m 0644 include/rte/abi.h $(DESTDIR)$(PREFIX)/include/rte/abi.h
	install -D -m 0644 include/rte/model.h $(DESTDIR)$(PREFIX)/include/rte/model.h
	install -D -m 0644 include/rte/register_io.h $(DESTDIR)$(PREFIX)/include/rte/register_io.h
	install -D -m 0644 include/rte/mmio.h $(DESTDIR)$(PREFIX)/include/rte/mmio.h
	install -D -m 0644 include/rte/ad936x.h $(DESTDIR)$(PREFIX)/include/rte/ad936x.h
	install -D -m 0644 include/rte/controller.h $(DESTDIR)$(PREFIX)/include/rte/controller.h

install-http: $(HTTPD)
	install -D -m 0755 $(HTTPD) $(DESTDIR)$(PREFIX)/sbin/rte-httpd

clean:
	$(RM) -r $(BUILD_DIR)

$(BUILD_DIR)/ad936x.o: src/ad936x.c include/rte/ad936x.h include/rte/model.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IIO_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/controller.o: src/controller.c include/rte/controller.h include/rte/ad936x.h include/rte/mmio.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IIO_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/mongoose.o: $(MONGOOSE_SOURCE) | $(BUILD_DIR)
	$(CC) $(MONGOOSE_CFLAGS) $(MONGOOSE_LIMIT_CFLAGS) \
		-O2 -std=gnu11 -c -o $@ $<

$(HTTPD): src/httpd.c $(CORE_LIBRARY) $(HTTP_OBJECTS) $(MONGOOSE_DEPS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IIO_CFLAGS) $(MONGOOSE_CFLAGS) $(LDFLAGS) \
		-o $@ src/httpd.c $(HTTP_OBJECTS) $(CORE_LIBRARY) \
		$(MONGOOSE_LINK) $(IIO_LIBS) -lpthread $(LDLIBS_CORE)

httpd: $(HTTPD)
