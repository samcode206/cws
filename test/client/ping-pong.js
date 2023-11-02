const WebSocket = require("ws");

const ws = new WebSocket("ws://[::1]:9919/", {
  perMessageDeflate: false,
  
});

ws.onopen = (ev) => {
  console.log("opened");
};

ws.onclose = (ev) => {
  // console.log("close", ev);
};

ws.onerror = (err) => {
  console.log("err", err);
};

ws.onmessage = (msg) => {
  console.info("msg");
};

ws.on("ping", (data) => {
  console.log("ping", data.toString());
});

ws.on("pong", (data) => {
  console.log("pong", data.toString());
});
ws.on("unexpected-response", (req, res) => {
  console.log(res);
});

process.stdin.on("data", (data) => {
  if (ws.OPEN) {
    for (let i = 0; i < 10; i++){
      ws.ping(`payload-${i}`, (err) => {
        if (err) {
          console.error(err);
        }
      });
    }
  } else if (ws.CONNECTING) {
    console.warn("please try later connecting...");
  } else {
    console.error("websocket is closed or closing!");
  }
});
