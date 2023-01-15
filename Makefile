CFLAGS = -g -O -Wall -Wextra

all: ifgraphd ifgraph

ifgraphd: src/ifgraphd.o
	$(CC) -o $@ $^

ifgraph: src/ifgraph.o src/grapher.o
	$(CC) -o $@ $^

clean:
	rm -f src/*.o ifgraphd ifgraph

