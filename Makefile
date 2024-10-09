prefix = /usr

all: modem-kick

PKGCONFIG_FLAGS=`pkg-config --libs --cflags glib-2.0 gobject-2.0 mm-glib`

modem-kick: modem-kick.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDCFLAGS) $^ $(PKGCONFIG_FLAGS) -o $@

install: modem-kick
	install -D modem-kick $(DESTDIR)$(prefix)/sbin/modem-kick
	install -D modem-kick.service $(DESTDIR)$(prefix)/lib/systemd/system/modem-kick.service

clean:
	-rm -f modem-kick

distclean: clean

uninstall:
	-rm -f $(DESTDIR)$(prefix)/bin/modem-kick

.PHONY: all install clean distclean uninstall
