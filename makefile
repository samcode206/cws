build:
	gcc ./src/base64.c ./src/ws.c -lcrypto -O3 -Wall --pedantic -o server
