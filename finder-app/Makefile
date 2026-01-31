CC := $(CROSS_COMPILE)gcc
CFLAGS := -Wall

all: writer

writer: writer.o
	$(CC) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o writer
