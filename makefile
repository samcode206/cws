build:
	gcc base64.c ws.c -lcrypto -O3 -Wall --pedantic -o server