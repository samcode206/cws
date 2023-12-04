const WebSocket = require("ws");

const ws = new WebSocket("ws://[::1]:9919/", {
  perMessageDeflate: false,
});

ws.onopen = (ev) => {
  console.log("opened");
};

ws.onerror = (err) => {
  console.log("err", err);
};

ws.on("close", (code, reason) => {
  console.log(
    "closed with code = ",
    code,
    "reason=",
    reason.toString().length ? reason.toString() : null
  );
});

ws.on("message", (msg, bin) => {
  console.log(
    "Received",
    bin ? " binary" : "text",
    "msg",
    "size",
    msg.length,
    msg.toString()
  );
});

ws.on("ping", (data) => {
  console.log("ping", data.toString());
  // ws.pong("hi");
  // ws.send("hello");
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
    let data = Buffer.from("*".repeat(i++));
    ws.send(data.toString(), (err) => {
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
