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

process.stdin.on("data", (chunk) => {
  if (ws.OPEN) {
    const data = chunk.toString('ascii');
    let fin = 0;
    if (data.includes('fin')){
      fin = 1;
    }
    ws.send(data, {
      fin: fin,
    }, (err) => {
      if (err) {
        console.error(err);
      }
    });

    ws.ping('hi');
  } else if (ws.CONNECTING) {
    console.warn("please try later connecting...");
  } else {
    console.error("websocket is closed or closing!");
  }
});


