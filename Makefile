PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs ${PACKAGES}` -lm
INCS = `pkg-config --cflags ${PACKAGES}`
CFLAGS = -Wall -O3 -flto
PREFIX = /usr/local
MANDIR = ${PREFIX}/share/man/man1

OBJS=fastcompmgr.o comp_rect.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

install: fastcompmgr
	@cp -t ${PREFIX}/bin fastcompmgr
	@[ -d "${MANDIR}" ] \
	  && cp -t "${MANDIR}" fastcompmgr.1

uninstall:
	@rm -f ${PREFIX}/fastcompmgr
	@rm -f ${MANDIR}/fastcompmgr.1

clean:
	rm -f $(OBJS) fastcompmgr

.PHONY: uninstall clean
