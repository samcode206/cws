CC=gcc
OS := $(shell uname -s)

ifeq ($(OS),Darwin)
    LD_CONFIG_COMMAND = 
else
    LD_CONFIG_COMMAND = sudo ldconfig
	SHARED_LIB_EXT = .so
endif

CFLAGS=-O3 -flto -march=native -mtune=native -Wpedantic -Wall -Wextra -Wsign-conversion -Wconversion -I./src
LIB_NAME=ws
SHARED_LIB=lib$(LIB_NAME)$(SHARED_LIB_EXT)
STATIC_LIB=lib$(LIB_NAME).a
SRC=./src/ws.c
HDR=ws.h
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

ifdef WS_TIMERS_DEFAULT_SZ
CFLAGS += -DWS_TIMERS_DEFAULT_SZ=$(WS_TIMERS_DEFAULT_SZ)
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
	$(LD_CONFIG_COMMAND)
	sudo install -m 644 ./src/$(HDR) $(INSTALL_INCLUDE_PATH)

uninstall:
	sudo rm -f $(INSTALL_LIB_PATH)/$(SHARED_LIB)
	sudo rm -f $(INSTALL_LIB_PATH)/$(STATIC_LIB)
	sudo rm -f $(INSTALL_INCLUDE_PATH)/$(HDR)
	$(LD_CONFIG_COMMAND)

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

multi_broadcast: install
	gcc ./examples/multi_broadcast.c -flto -lws -O3  -march=native -mtune=native -Wall --pedantic -o server

autobahn: install
	gcc ./test/autobahn/autobahn.c -flto -lws -O3  -march=native -mtune=native -Wall --pedantic -o server

async_task:
	gcc ./src/*.c ./test/e2e/async_task.c -lz -O3 -march=native -mtune=native -Wall --pedantic -o server

