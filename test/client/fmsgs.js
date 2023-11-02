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

let i = 1;
process.stdin.on("data", (chunk) => {
  if (ws.OPEN) {
    const data = chunk.toString('ascii');
    let fin = 0;
    if (data.includes('fin')){
      fin = 1;
    }
    ws.send('*'.replace('\n', ''), {
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


