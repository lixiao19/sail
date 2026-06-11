CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror
LDLIBS ?= -libverbs -lpthread

COMMON = sail_common.o

.PHONY: all clean

all: sail_sender sail_receiver

sail_sender: sail_sender.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

sail_receiver: sail_receiver.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c sail_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) sail_sender sail_receiver *.o
