CFLAGS = -g -O -Wall -Wextra -MMD -MP

DISTFILES =\
LICENSE \
README.md \
Makefile \
src/ifgraph.h \
src/ifgraphd.c \
src/ifgraph.c \
src/hsv.h \
src/grapher.c \
src/grapher.h \
images \
ifgraph.service \

# Two binaries: one for back-end recording, and one for front-end displaying.
all: ifgraphd ifgraph

# Back-end binary
ifgraphd: src/ifgraphd.o
	$(CC) -o $@ $^ -lrt

# Front-end binary
ifgraph: src/ifgraph.o src/grapher.o
	$(CC) -o $@ $^ -lrt

clean:
	rm -f src/*.o ifgraphd ifgraph

dist-clean: clean
	rm -f src/*.d

install: ifgraph ifgraphd
	install -d ${DESTDIR}/usr/bin
	install -m 755 ifgraph  ${DESTDIR}/usr/bin/
	install -m 755 ifgraphd ${DESTDIR}/usr/bin/
	install -d ${DESTDIR}/lib/systemd/system
	install -m 644 ifgraph.service  ${DESTDIR}/lib/systemd/system/

uninstall:
	rm -f ${DESTDIR}/usr/bin/ifgraph
	rm -f ${DESTDIR}/usr/bin/ifgraphd

# debuild needs an original tar ball to work off.
tarball:
	tar cvzf ../ifgraph_1.0.orig.tar.gz $(DISTFILES)

# upload the source package to the author's personal package archive.
packageupload:
	debuild -S
	dput ppa:b-stolk/ppa ../energygraph_1.0-1_source.changes

# automatically generated dependencies.
-include $(src/ifgraphd.d src/ifgraph.d src/grapher.d)

