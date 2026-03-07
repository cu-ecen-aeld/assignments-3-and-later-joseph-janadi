CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -pthread
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
