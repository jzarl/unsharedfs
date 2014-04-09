unshared: unshared.o
	gcc -g -o unshared unshared.o `pkg-config fuse --libs`

unshared.o : unshared.c
	gcc -g -Wall `pkg-config fuse --cflags` -c unshared.c
clean:
	rm -f unshared *.o
