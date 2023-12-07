CC=gcc
CFLAGS=-O3 -march=native -mtune=native -Wall --pedantic -I./src
LIB_NAME=ws
SHARED_LIB=lib$(LIB_NAME).so
STATIC_LIB=lib$(LIB_NAME).a
SRC=./src/ws.c
HDR=./src/ws.h
SOBJ=$(SRC:.c=.o)
DOBJ=$(SRC:.c=.d.o)

PREFIX=/usr/local
INSTALL_LIB_PATH=$(PREFIX)/lib
INSTALL_INCLUDE_PATH=$(PREFIX)/include

.PHONY: all clean install uninstall

all: $(SHARED_LIB) $(STATIC_LIB)


$(SOBJ): $(SRC)
	$(CC) $(CFLAGS) -c $< -o $@


$(DOBJ): $(SRC)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@


$(SHARED_LIB): $(DOBJ)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $^


$(STATIC_LIB): $(SOBJ)
	ar rcs $@ $^

install: clean all
	sudo install -m 644 $(SHARED_LIB) $(INSTALL_LIB_PATH)
	sudo install -m 644 $(STATIC_LIB) $(INSTALL_LIB_PATH)
	sudo ldconfig
	sudo install -m 644 $(HDR) $(INSTALL_INCLUDE_PATH)

uninstall:
	sudo rm -f $(INSTALL_LIB_PATH)/$(SHARED_LIB)
	sudo rm -f $(INSTALL_LIB_PATH)/$(STATIC_LIB)
	sudo rm -f $(INSTALL_INCLUDE_PATH)/ws.h
	sudo ldconfig

clean:
	rm -f $(SOBJ) $(DOBJ) $(SHARED_LIB) $(STATIC_LIB)




# -flto

echo:
	gcc ./examples/echo.c -lws -lcrypto -lz -O3 -march=native -mtune=native -Wall --pedantic -o server

fragmented_send:
	gcc ./test/fragmented_send.c -lws -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server

online:
	gcc ./examples/online.c -lws -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server

broadcast:
	gcc ./examples/broadcast.c -lws -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server

autobahn:
	gcc ./test/autobahn/autobahn.c -lws -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server

