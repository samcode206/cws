example:
	gcc ./src/base64.c ./src/pool.c ./src/ws.c ./src/example.c -lcrypto -O3  -march=native -mtune=native -Wall -g --pedantic -o server
