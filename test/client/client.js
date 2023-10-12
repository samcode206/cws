const WebSocket = require("ws");

const ws = new WebSocket("ws://127.0.0.1:9919/", {
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


ws.on("message", (msg, bin) => {
  console.log(bin ? " binary": "text", "msg",  msg.toString());
})

ws.on("ping", (data) => {
  console.log("ping", data.toString());
});

ws.on("pong", (data) => {
  console.log("pong", data.toString());
});

ws.on("unexpected-response", (req, res) => {
  console.log(res);
});

let i = 1;
process.stdin.on("data", (data) => {
  if (ws.OPEN) {
    let data = Buffer.from('*'.repeat(i++));
    ws.send(data, (err) => {
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
