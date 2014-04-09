CFLAGS = -g -O2 -Wall `pkg-config fuse --cflags`
LDFLAGS = `pkg-config fuse --libs`

# enable syslog:
CFLAGS += -DHAVE_SYSLOG

all: unshared
#unshared: unshared.o
#	gcc -g -o unshared unshared.o `pkg-config fuse --libs`
#
#unshared.o : unshared.c
#	gcc -g -Wall `pkg-config fuse --cflags` -c unshared.c

clean:
	rm -f unshared *.o

