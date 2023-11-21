example:
	gcc ./src/base64.c ./src/buffer.c ./src/ws.c ./src/example.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server
