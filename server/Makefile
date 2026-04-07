CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -pthread \
		  -I../aesd-char-driver
USE_AESD_CHAR_DEVICE ?= 1
ifeq ($(USE_AESD_CHAR_DEVICE), 1)
	CFLAGS += -DUSE_AESD_CHAR_DEVICE
endif
LDFLAGS ?= -pthread -lrt
SRCS := aesdsocket.c
OBJS := $(SRCS:.c=.o)
TARGET := aesdsocket

.PHONY: all default clean
all: $(TARGET)

default: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(OBJS) $(TARGET)
