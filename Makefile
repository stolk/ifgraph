CFLAGS = -g -O -Wall -Wextra

all: ifgraphd ifgraph

ifgraphd: src/ifgraphd.o
	$(CC) -o $@ $^ -lrt

ifgraph: src/ifgraph.o src/grapher.o
	$(CC) -o $@ $^ -lrt

clean:
	rm -f src/*.o ifgraphd ifgraph

