const WebSocket = require("ws");

const ws = new WebSocket("ws://127.0.0.1:9919/", {
  perMessageDeflate: false,
});

ws.onopen = (ev) => {
  console.log("opened");
};

ws.onerror = (err) => {
  console.log("err", err);
};

ws.onmessage = (msg) => {
  console.info("msg");
};

ws.on("close", (code, reason) => {
  console.log("close", code, reason);
});

ws.on("ping", (data) => {
  console.log("ping", data.toString());
});

ws.on("pong", (data) => {
  console.log("pong", data.toString());
  ws.close(1000, 'done');
});
ws.on("unexpected-response", (req, res) => {
  console.log(res);
});

process.stdin.on("data", (data) => {
  if (ws.OPEN) {
    ws.ping(data.toString("ascii"), (err) => {
      if (err) {
        console.error(err);
      }
    });
  } else if (ws.CONNECTING) {
    console.warn("please try later connecting...");
  } else {
    console.error("websocket is closed or closing!");
  }
});
