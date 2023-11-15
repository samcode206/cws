const WebSocket = require("ws");

const ws = new WebSocket("ws://[::1]:9919/", {
  perMessageDeflate: true,
});

ws.onopen = (ev) => {
  console.log("opened");
};


ws.onerror = (err) => {
  console.log("err", err);
};


ws.on("close", (code, reason) => {
  console.log('closed with code = ', code, 'reason=', reason.toString().length ? reason.toString() : null);
})

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


let textData = 'abcdefghijklmnopzrstuvwxyz';

process.stdin.on("data", () => {
  if (ws.OPEN) {
    ws.send(textData.repeat(512), {
        compress: true,
    },  (err) => {
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
