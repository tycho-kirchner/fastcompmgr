PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs ${PACKAGES}` -lm
INCS = `pkg-config --cflags ${PACKAGES}`
CFLAGS = -Wall -O3 -flto
PREFIX = /usr/local
BINDIR = ${PREFIX}/bin
MANDIR = ${PREFIX}/share/man/man1

OBJS=fastcompmgr.o comp_rect.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

install: fastcompmgr
	@install -d $(BINDIR)
	@install -m 755 fastcompmgr $(BINDIR)
	@[ -d "${MANDIR}" ] && install -m 644 fastcompmgr.1 $(MANDIR) || true

uninstall:
	@rm -f $(BINDIR)/fastcompmgr
	@rm -f $(MANDIR)/fastcompmgr.1

clean:
	rm -f $(OBJS) fastcompmgr

.PHONY: install uninstall clean
