PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs ${PACKAGES}` -lm
INCS = `pkg-config --cflags ${PACKAGES}`
CFLAGS = -Wall -O3 -flto
PREFIX = /usr/local
MANDIR = ${PREFIX}/share/man/man1

OBJS=fastcompmgr.o comp_rect.o cm-root.o cm-global.o cm-util.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

install: fastcompmgr
	@mkdir -p "${PREFIX}/bin"
	@cp -t "${PREFIX}/bin" fastcompmgr
	@mkdir -p "${MANDIR}"
	@cp -t "${MANDIR}" fastcompmgr.1

uninstall:
	@rm -f "${PREFIX}/bin/fastcompmgr"
	@rm -f "${MANDIR}/fastcompmgr.1"

clean:
	rm -f $(OBJS) fastcompmgr

.PHONY: uninstall clean
