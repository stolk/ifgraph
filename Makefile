CFLAGS = -g -O -Wall -Wextra

ifgraphd: src/ifgraphd.o
	$(CC) -o $@ src/ifgraphd.o


