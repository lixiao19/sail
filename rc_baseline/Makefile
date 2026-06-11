CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror
LDLIBS ?= -libverbs -lpthread

COMMON = rc_common.o

.PHONY: all clean

all: rc_sender rc_receiver

rc_sender: rc_sender.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

rc_receiver: rc_receiver.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c rc_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) rc_sender rc_receiver *.o
