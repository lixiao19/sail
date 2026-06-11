CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Wshadow -Wstrict-prototypes -Werror
LDLIBS ?= -libverbs -lpthread

COMMON = flor_common.o

.PHONY: all clean

all: flor_sender flor_receiver

flor_sender: flor_sender.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

flor_receiver: flor_receiver.o $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c flor_common.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	$(RM) flor_sender flor_receiver *.o
