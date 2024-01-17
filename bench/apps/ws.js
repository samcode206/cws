const serverStart = () => {
    const Ws = require("ws");
  
    const server = new Ws.Server({
      port: 9004,
      perMessageDeflate: false,
      clientTracking: false,
    });
  
    server.on("listening", () => {
      console.log("ws server listening", 9004);
    });
  
    server.on("error", (err) => {
      console.log("ws server error", err);
    });
  
    server.on("connection", (ws) => {
      ws.on("message", (message) => {
        ws.send(message);
      });
  
      ws.on("close", () => {});
  
      ws.on("error", (err) => {});
    });
  };
  
  let num = 1;
  if (process.argv[2].includes("--cpus") || process.argv[2].includes("-cpus")) {
    num = process.argv[3] || 1;
  }
  
  if (num > 1) {
    const cluster = require("cluster");
  
    if (cluster.isMaster) {
      console.log("starting " + num + " ws servers");
      for (let i = 0; i < num; i++) {
        cluster.fork();
      }
    } else {
      serverStart();
    }
  } else {
    serverStart();
  }
  