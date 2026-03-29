PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs ${PACKAGES}` -lm
INCS = `pkg-config --cflags ${PACKAGES}`
CFLAGS ?= -O2 -flto -pipe
CFLAGS += -Wall -fno-plt
PREFIX = /usr/local
MANDIR = ${PREFIX}/share/man/man1

OBJS=fastcompmgr.o comp_rect.o cm-root.o cm-global.o cm-util.o cm-window.o cm-event.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

install: fastcompmgr
	@mkdir -p "${PREFIX}/bin"
	@install -m755 fastcompmgr "${PREFIX}/bin"
	@mkdir -p "${MANDIR}"
	@install -m644 fastcompmgr.1 "${MANDIR}"

uninstall:
	@rm -f "${PREFIX}/bin/fastcompmgr"
	@rm -f "${MANDIR}/fastcompmgr.1"

clean:
	rm -f $(OBJS) fastcompmgr

.PHONY: uninstall clean
