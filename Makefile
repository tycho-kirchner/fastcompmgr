PACKAGES = x11 xcomposite xfixes xdamage xrender
LIBS = `pkg-config --libs ${PACKAGES}` -lm
INCS = `pkg-config --cflags ${PACKAGES}`
CFLAGS = -Wall -O3 -flto
BIN = /usr/local/bin

# This should get all the possible man's path
# And split the char ':' into a new line
MANPATH = $(shell manpath | tr ':' '\n')

OBJS=fastcompmgr.o comp_rect.o cm-root.o cm-global.o

.c.o:
	$(CC) $(CFLAGS) $(INCS) -c $*.c

fastcompmgr: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

install: fastcompmgr
	@echo "[=] Installing fastcompmgr"
	@cp -t ${BIN} fastcompmgr
	@echo "[+] fastcompmgr copied to bin!"
	@for dir in $(MANPATH); do \
		MANDIR=$$(find "$$dir" -type d -path "*/man/man1"); \
		if [ -n "$$MANDIR" ]; then \
			echo "[=] Creating fastcompmgr man: $$MANDIR"; \
			cp -t "$$MANDIR" fastcompmgr.1; \
			echo "[+] fastcompmgr man created!"; \
			break; \
		fi \
	done
	@echo "[!] fastcompmgr sucessfully installed!"

uninstall:
	@rm -f ${BIN}/fastcompmgr
	@for dir in $(MANPATH); do \
		MANDIR=$$(find "$$dir" -type d -path "*/man/man1"); \
		if [ -n "$$MANDIR" ]; then \
			rm -f "$${MANDIR}/fastcompmgr.1"; \
			break; \
		fi \
	done

clean:
	rm -f $(OBJS) fastcompmgr

.PHONY: uninstall clean
