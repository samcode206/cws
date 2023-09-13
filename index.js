const net = require("net");

const socket = net.createConnection(9919,"0.0.0.0");

socket.on("connect", () => {
  socket.on("data", (data) => {
    console.log(data.length , data.indexOf('\0'))
    console.log(data.toString());
  });

  socket.write(
    "GET /chat HTTP/1.1\r\nHost: example.com:8000\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
  );
});
