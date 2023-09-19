const net = require("net");

const socket = net.createConnection(9919, "0.0.0.0");

const results =
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";

socket.on("connect", () => {
  socket.on("data", (data) => {
    console.assert(data.length === 129 && data.indexOf("\0") === -1);
    console.assert(data.toString() === results);
    console.log(data.toString());
    socket.end("thanks");
  });

  socket.write(
    "GET /chat HTTP/1.1\r\nHost: example.com:8000\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
  );
});
