online:
	gcc ./src/** ./examples/online.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


autobahn:
	gcc ./src/** ./examples/autobahn.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


broadcast: 
	gcc ./src/** ./examples/broadcast.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server
