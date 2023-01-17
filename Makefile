CFLAGS = -g -O -Wall -Wextra

all: ifgraphd ifgraph

ifgraphd: src/ifgraphd.o
	$(CC) -o $@ $^ -lrt

ifgraph: src/ifgraph.o src/grapher.o
	$(CC) -o $@ $^ -lrt

install: all
	install -D ifgraphd $(DESTDIR)/usr/local/bin/ifgraphd
	install -D ifgraph $(DESTDIR)/usr/local/bin/ifgraph

# one time start
start: install
	ifgraphd &
	ifgraph


systemd: install
	install -D -m 644 ifgraphd.service /etc/systemd/system/ifgraphd.service
	systemctl daemon-reload
	systemctl enable ifgraphd.service
	systemctl start ifgraphd.service

initd: install
	install -D -m 755 ifgraphd.initd $(DESTDIR)/etc/init.d/ifgraphd
	rc-update add ifgraphd
	/etc/init.d/ifgraphd start


uninstall:
	rm $(DESTDIR)/usr/local/bin/ifgraphd $(DESTDIR)/usr/local/bin/ifgraph

unsystemd:
	systemctl stop ifgraphd.service
	systemctl disable ifgraphd.service
	systemctl daemon-reload
	rm /etc/systemd/system/ifgraphd.service

uninitd:
	/etc/init.d/ifgraphd stop
	rc-update del ifgraphd
	rm $(DESTDIR)/etc/init.d/ifgraphd

clean:
	rm -f src/*.o ifgraphd ifgraph

