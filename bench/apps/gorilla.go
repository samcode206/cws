// server.go
package main

import (
	"log"
	"net/http"
	"runtime"
	"fmt"
	
	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
	ReadBufferSize: 1024 * 512,
	WriteBufferSize: 1024 * 512,
}

func echoHandler(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		log.Println(err)
		return
	}
	defer conn.Close()

	for {
		messageType, p, err := conn.ReadMessage()
		if err != nil {
			// log.Println(err)
			return
		}
		
		if err := conn.WriteMessage(messageType, p); err != nil {
			// log.Println(err)
			return
		}
	}
}

func main() {
	// GOMAXPROCS=<whatever> ./name-of-exe
	fmt.Println("Max Procs", runtime.GOMAXPROCS(0))


	fmt.Println("starting on port 9003");

	http.HandleFunc("/", echoHandler)
	log.Fatal(http.ListenAndServe("[::1]:9003", nil))
}
