# tools/ifd-hdus/Makefile — PC/SC IFD handler for the HDUS B-CAS slot.
# Builds libhdus_ifd.so (pcsc-lite IFDHandler v3, see hdus_ifd.c) plus the
# hdus_card_test real-hardware verification binary.
#
# The handler needs only the pcsc-lite headers (no -lpcsclite: pcscd
# dlopen()s the IFDH* symbols and provides the PC/SC runtime).

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2
CPPFLAGS += -I/usr/include/PCSC -I. -I..
LDLIBS  += -lpthread

all: libhdus_ifd.so hdus_card_test

libhdus_ifd.so: hdus_card.o hdus_ifd.o
	$(CC) $(CFLAGS) -shared -o $@ hdus_card.o hdus_ifd.o $(LDLIBS)

hdus_card.o: hdus_card.c hdus_card.h ../card_sbox.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c -o $@ $<

hdus_ifd.o: hdus_ifd.c hdus_card.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c -o $@ $<

# Test binary: same transport, with the __MAIN__ section of hdus_card.c.
hdus_card_test: hdus_card.c hdus_card.h ../card_sbox.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHDUS_CARD_TEST_MAIN -o $@ hdus_card.c $(LDLIBS)

install: libhdus_ifd.so
	sudo install -m0755 libhdus_ifd.so /usr/lib/pcsc/drivers/libhdus_ifd.so
	@echo "Next: merge reader.conf.example into /etc/reader.conf, then"
	@echo "  sudo systemctl restart pcscd   (or: sudo pcscd --foreground)"

clean:
	rm -f libhdus_ifd.so hdus_card_test hdus_card.o hdus_ifd.o

.PHONY: all install clean
