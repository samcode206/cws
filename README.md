# cws


### Example Sever

```c

#include "ws.h"

void on_open(ws_conn_t *c) { ws_conn_ping(ws_conn_server(c), c, "welcome", 8); }

void on_ping(ws_conn_t *c, void *msg, size_t n) {
  printf("on_ping: %s\n", (char *)msg);
  int stat = ws_conn_pong(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("pong sent\n");
  } else {
    printf("partial pong sent or an error occurred waiting for <on_ws_drain | "
           "on_ws_disconnect>\n");
  }
}

void on_pong(ws_conn_t *c, void *msg, size_t n) {
  printf("on_pong: %s\n", (char *)msg);
}

void on_msg(ws_conn_t *c, void *msg, size_t n, bool bin) {
  msg_unmask(msg, msg, n);
  printf("on_msg: %s\n", (char *)msg);
  int stat = ws_conn_send_txt(ws_conn_server(c), c, msg, n);
  if (stat == 1) {
    printf("msg sent\n");
  } else {
    printf("partial send or an error occurred waiting for <on_ws_drain | "
           "on_ws_disconnect>\n");
  }
}

void on_close(ws_conn_t *ws_conn, int reason) { printf("on_close\n"); }

void on_disconnect(ws_conn_t *ws_conn, int err) { printf("on_disconnect\n"); }

void on_drain(ws_conn_t *ws_conn) { printf("on_drain\n"); }

void on_server_err(ws_server_t *s, int err) {
  fprintf(stderr, "on_server_err: %s\n", strerror(err));
}

int main(void) {
  const size_t max_events = 1024;
  const uint16_t port = 9919;
  const int backlog = 1024;
  struct ws_server_params sp = {.addr = INADDR_ANY,
                                .port = port,
                                .max_events = max_events,
                                .on_ws_open = on_open,
                                .on_ws_msg = on_msg,
                                .on_ws_ping = on_ping,
                                .on_ws_pong = on_pong,
                                .on_ws_drain = on_drain,
                                .on_ws_close = on_close,
                                .on_ws_disconnect = on_disconnect,
                                .on_ws_err = on_server_err};

  int ret = 0;
  ws_server_t *s = ws_server_create(&sp, &ret);

  if (ret < 0) {
    fprintf(stderr, "ws_server_create: %d\n", ret);
    exit(1);
  }

  printf("websocket server starting on port : %d\n", sp.port);

  ret = ws_server_start(s, backlog);

  exit(ret);
}

```


 Performance counter stats for process id '4860':

         30,485.82 msec task-clock                       #    0.759 CPUs utilized          
             1,216      context-switches                 #   39.887 /sec                   
               130      cpu-migrations                   #    4.264 /sec                   
               102      page-faults                      #    3.346 /sec                   
   145,500,558,675      cycles                           #    4.773 GHz                      (38.45%)
   147,437,018,837      instructions                     #    1.01  insn per cycle           (46.15%)
    29,212,906,707      branches                         #  958.246 M/sec                    (53.84%)
       203,692,730      branch-misses                    #    0.70% of all branches          (61.53%)
   651,294,942,143      slots                            #   21.364 G/sec                    (69.22%)
    49,996,695,721      topdown-retiring                 #      6.9% Retiring                (69.22%)
   439,304,823,720      topdown-bad-spec                 #     60.9% Bad Speculation         (69.22%)
   154,021,449,204      topdown-fe-bound                 #     21.4% Frontend Bound          (69.22%)
    77,703,857,456      topdown-be-bound                 #     10.8% Backend Bound           (69.22%)
    43,091,417,398      L1-dcache-loads                  #    1.413 G/sec                    (69.21%)
     2,381,560,816      L1-dcache-load-misses            #    5.53% of all L1-dcache accesses  (69.22%)
       796,296,677      LLC-loads                        #   26.120 M/sec                    (69.23%)
       138,342,548      LLC-load-misses                  #   17.37% of all LL-cache accesses  (69.24%)
   <not supported>      L1-icache-loads                                             
    11,324,580,710      L1-icache-load-misses                                                (30.79%)
    43,226,892,126      dTLB-loads                       #    1.418 G/sec                    (30.78%)
        26,975,103      dTLB-load-misses                 #    0.06% of all dTLB cache accesses  (30.77%)
   <not supported>      iTLB-loads                                                  
       179,333,951      iTLB-load-misses                                                     (30.76%)
   <not supported>      L1-dcache-prefetches                                        
   <not supported>      L1-dcache-prefetch-misses                                   

      40.159628704 seconds time elapsed



og :




         36,956.17 msec task-clock                       #    0.629 CPUs utilized          
             1,799      context-switches                 #   48.679 /sec                   
               235      cpu-migrations                   #    6.359 /sec                   
               102      page-faults                      #    2.760 /sec                   
   175,089,651,835      cycles                           #    4.738 GHz                      (38.47%)
   177,126,461,975      instructions                     #    1.01  insn per cycle           (46.16%)
    35,062,636,327      branches                         #  948.763 M/sec                    (53.86%)
       244,887,965      branch-misses                    #    0.70% of all branches          (61.55%)
   777,878,116,261      slots                            #   21.049 G/sec                    (69.24%)
    59,409,557,303      topdown-retiring                 #      7.0% Retiring                (69.24%)
   552,140,937,423      topdown-bad-spec                 #     65.5% Bad Speculation         (69.24%)
   178,850,659,661      topdown-fe-bound                 #     21.2% Frontend Bound          (69.24%)
    52,941,010,682      topdown-be-bound                 #      6.3% Backend Bound           (69.24%)
    51,714,213,102      L1-dcache-loads                  #    1.399 G/sec                    (69.22%)
     2,871,079,261      L1-dcache-load-misses            #    5.55% of all L1-dcache accesses  (69.21%)
       961,751,194      LLC-loads                        #   26.024 M/sec                    (69.23%)
       166,488,430      LLC-load-misses                  #   17.31% of all LL-cache accesses  (69.22%)
   <not supported>      L1-icache-loads                                             
    13,696,153,568      L1-icache-load-misses                                                (30.78%)
    51,771,739,836      dTLB-loads                       #    1.401 G/sec                    (30.79%)
        33,487,946      dTLB-load-misses                 #    0.06% of all dTLB cache accesses  (30.77%)
   <not supported>      iTLB-loads                                                  
       215,909,187      iTLB-load-misses                                                     (30.78%)
   <not supported>      L1-dcache-prefetches                                        
   <not supported>      L1-dcache-prefetch-misses                                   

      58.791960480 seconds time elapsed


new:

sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.123034237s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.145617505s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.114693386s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.089114975s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.007836113s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.030142894s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.074117242s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.083433199s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.090510514s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ ./wsbench 
Sent and received 1000000 messages of size 512 in 3.157616364s using 1000 goroutines
sameem@workerDT:~/Documents/projects/wsbench$ 

         30,543.69 msec task-clock                       #    0.736 CPUs utilized          
             1,832      context-switches                 #   59.980 /sec                   
               200      cpu-migrations                   #    6.548 /sec                   
               110      page-faults                      #    3.601 /sec                   
   145,535,095,086      cycles                           #    4.765 GHz                      (38.46%)
   148,149,594,396      instructions                     #    1.02  insn per cycle           (46.15%)
    29,293,888,738      branches                         #  959.081 M/sec                    (53.84%)
       204,236,491      branch-misses                    #    0.70% of all branches          (61.54%)
   654,613,402,821      slots                            #   21.432 G/sec                    (69.23%)
    51,420,423,139      topdown-retiring                 #      7.3% Retiring                (69.23%)
   459,512,937,667      topdown-bad-spec                 #     65.6% Bad Speculation         (69.23%)
   153,645,052,516      topdown-fe-bound                 #     21.9% Frontend Bound          (69.23%)
    35,990,702,986      topdown-be-bound                 #      5.1% Backend Bound           (69.23%)
    43,246,183,089      L1-dcache-loads                  #    1.416 G/sec                    (69.23%)
     2,410,256,512      L1-dcache-load-misses            #    5.57% of all L1-dcache accesses  (69.26%)
       805,902,122      LLC-loads                        #   26.385 M/sec                    (69.25%)
       136,423,406      LLC-load-misses                  #   16.93% of all LL-cache accesses  (69.25%)
   <not supported>      L1-icache-loads                                             
    11,335,926,279      L1-icache-load-misses                                                (30.77%)
    43,145,277,043      dTLB-loads                       #    1.413 G/sec                    (30.74%)
        27,314,536      dTLB-load-misses                 #    0.06% of all dTLB cache accesses  (30.75%)
   <not supported>      iTLB-loads                                                  
       178,337,848      iTLB-load-misses                                                     (30.75%)
   <not supported>      L1-dcache-prefetches                                        
   <not supported>      L1-dcache-prefetch-misses                                   

      41.485116315 seconds time elapsed

old:
