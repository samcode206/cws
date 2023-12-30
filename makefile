CC=gcc
CFLAGS=-O3 -flto -march=native -mtune=native -Wpedantic -Wall -Wextra -Wsign-conversion -Wconversion -I./src
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

ifdef WITH_COMPRESSION
CFLAGS += -DWITH_COMPRESSION
endif

ifdef NO_DEBUG
CFLAGS += -DNDEBUG
endif

all: $(SHARED_LIB) $(STATIC_LIB)


$(SOBJ): $(SRC)
	$(CC) $(CFLAGS) -c $< -o $@


$(DOBJ): $(SRC)
	$(CC) $(CFLAGS) -fPIC -c $< -o $@


$(SHARED_LIB): $(DOBJ)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ $^ $(if $(WITH_COMPRESSION),-lz)


$(STATIC_LIB): $(SOBJ)
	ar rcs $@ $^

install: clean uninstall all
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




echo: install
	gcc ./examples/echo.c -flto -lws -O3 -march=native -mtune=native -Wall --pedantic -o server

multi_echo: install
	gcc ./examples/multi_echo.c -flto -lws -O3 -march=native -mtune=native -Wall --pedantic -o server

online: install
	gcc ./examples/online.c -flto -lws -O3  -march=native -mtune=native -Wall --pedantic -o server

broadcast: install
	gcc ./examples/broadcast.c -flto -lws -O3  -march=native -mtune=native -Wall --pedantic -o server

autobahn: install
	gcc ./test/autobahn/autobahn.c -flto -lws -O3  -march=native -mtune=native -Wall --pedantic -o server

timers: install
	gcc ./examples/timers.c -flto -lws -O3 -march=native -mtune=native -Wall --pedantic -o server

timers2: install
	gcc ./examples/timers2.c -flto -lws -O3 -march=native -mtune=native -Wall --pedantic -o server

async_task:
	gcc ./src/*.c ./test/e2e/async_task.c -lz O3 -march=native -mtune=native -Wall --pedantic -o server

