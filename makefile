online:
	gcc ./src/** ./examples/online.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


autobahn:
	gcc ./src/** ./test/autobahn/autobahn.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


broadcast: 
	gcc ./src/** ./examples/broadcast.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server

interval:
	gcc ./src/** ./examples/interval.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


echo: 
	gcc ./src/** ./examples/echo.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server


fragmented_send:
	gcc ./src/** ./examples/fragmented_send.c -lcrypto -lz -O3  -march=native -mtune=native -Wall --pedantic -o server
