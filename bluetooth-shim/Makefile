# webos-bt-shim — LD_PRELOAD HID gamepad/mouse shim for PmBtEngine (webOS 3.0.5)
#
# Cross build uses the Palm PDK toolchain (gcc 4.3.3, EABI softfp) which matches
# the shipped webOS userspace ABI exactly.  The shim does no floating point.

PDK    ?= /opt/PalmPDK
CROSS  ?= $(PDK)/arm-gcc/bin/arm-none-linux-gnueabi-
CC      = $(CROSS)gcc

TARGET  = libpmbtgamepad.so
SRCDIR  = src
OBJS    = $(SRCDIR)/shim.o $(SRCDIR)/hid_parser.o $(SRCDIR)/uinput_dev.o \
          $(SRCDIR)/wiimote.o $(SRCDIR)/log.o

CFLAGS  = -std=gnu99 -O2 -fPIC -Wall -Wextra -fvisibility=hidden \
          -march=armv7-a -marm
LDFLAGS = -shared -Wl,-soname,$(TARGET)
LIBS    = -ldl -lpthread

.PHONY: all clean host-test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)
	@echo "built $@:"; file $@ 2>/dev/null || true

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) test/test_parser test/test_ds4

# Host-side unit tests of the HID parser + emitter (no device needed).
host-test:
	cc -std=gnu99 -O2 -Wall -Isrc -o test/test_parser \
	   test/test_parser.c src/hid_parser.c src/uinput_dev.c src/log.c -lpthread 2>/dev/null || \
	cc -std=gnu99 -O2 -Wall -Isrc -o test/test_parser \
	   test/test_parser.c src/hid_parser.c src/uinput_dev.c src/log.c
	cc -std=gnu99 -O2 -Wall -Isrc -o test/test_ds4 \
	   test/test_ds4.c src/hid_parser.c src/uinput_dev.c src/log.c
	./test/test_parser
	./test/test_ds4
