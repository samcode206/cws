package main

import (
	"fmt"
	"strings"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
)

var id = 0
var topic string

func main() {
	c, _, err := websocket.DefaultDialer.Dial("ws://localhost:9919/", nil)
	if err != nil {
		fmt.Println("dial:", err)
		return
	}
	defer c.Close()

	go func() {
		data := []byte(strings.Repeat("a", 12))


		for {
			err := c.WriteMessage(websocket.TextMessage, data)
			if err != nil {
				fmt.Println(err)
			}
		}

	}()

	var msgs int64 = 0

	go func() {

		for {
			_, _, err := c.ReadMessage()
			if err != nil {
				fmt.Println("read:", err)
				return
			}

			atomic.AddInt64(&msgs, 1)
		}
	}()

	go func() {
		var prev int64 = 0
		for {
			time.Sleep(time.Second)
			read := atomic.LoadInt64(&msgs)
			fmt.Println("msgs per second", read-prev)
			prev = read
		}

	}()

	time.Sleep(time.Hour)

}
