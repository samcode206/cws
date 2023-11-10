const net = require("net");

const socket = net.createConnection(9919, "::1");

socket.setNoDelay(true);

const results =
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nServer: cws\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";

socket.on("connect", async () => {
  socket.on("data", (data) => {
    console.assert(data.length === results.length && data.indexOf("\0") === -1);
    console.assert(data.toString() === results);
    console.log(data.toString());
    // socket.end("thanks");
  });

  const requestBuffer = Buffer.from(
    "GET /chat HTTP/1.1\r\nHost: example.com:8000\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
  );

  for (const b of requestBuffer.toString()) {
    // console.log(b);
    socket.write(Buffer.from(b));
  }

  // let j = 4;
  // while (j--){
  //   console.log(socket.write(Buffer.allocUnsafe(1024)))
  // }



  // let i = 0;

  // while (true){
  //   if (i + 16 < requestBuffer.length){
  //     socket.write(requestBuffer.subarray(i, i+16));
  //     i+=16;
  //   } else {
  //     socket.write(requestBuffer.subarray(i, requestBuffer.length));
  //     break;
  //   }
  // }
});

socket.on("end", (e) => {
  console.log("end", e);
});

socket.on('close', (e) => {
  console.log(e);
})