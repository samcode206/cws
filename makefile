example:
	gcc ./src/base64.c ./src/pool.c ./src/ws.c ./src/example.c -lcrypto -O3 -Wall -march=native -mtune=native --pedantic -o server
