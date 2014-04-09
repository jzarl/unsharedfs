unshared: unshared.o
	gcc -g -o unshared unshared.o `pkg-config fuse --libs`

unshared.o : unshared.c params.h
	gcc -g -Wall `pkg-config fuse --cflags` -c unshared.c

bbfs : bbfs.o log.o
	gcc -g -o bbfs bbfs.o log.o `pkg-config fuse --libs`

bbfs.o : bbfs.c log.h params.h
	gcc -g -Wall `pkg-config fuse --cflags` -c bbfs.c

log.o : log.c log.h params.h
	gcc -g -Wall `pkg-config fuse --cflags` -c log.c

clean:
	rm -f bbfs unshared *.o
