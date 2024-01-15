#include "../../src/ws.h"
#include "../wsockutil.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#else
#include <sys/event.h>
#endif

/**
    description:
        create a server and wait for incoming messages
        when a message is received take the msg content
        and send it to one of the test workers

        create client threads that sends websocket
        messages to the server

        once a test worker receives a message it will simulate some kind of
        blocking/file IO work and then schedule a callback to be executed
        in the server to send a response back to the msg sender
*/

#define NUM_WORKERS 4
#define NUM_CLIENTS 32
#define EXPECTED_TASK_COMPLETIONS NUM_CLIENTS

#define PORT 9919
#define ADDR "::1"

typedef struct task {
  struct task *nxt;
  void *ctx;
  bool shutdown;
  char msg[];
} task_t;

struct task_q {
  pthread_mutex_t mu;
  int channel;
  task_t *task;
};

_Atomic int UEVID = 2048;
_Atomic unsigned long tasks_completed = 0;

ws_server_t *srv;

struct timespec rand_duration(long atleast, long mrand) {
  struct timespec ts = {
      .tv_nsec = atleast + (rand() % 10 * mrand),
      .tv_sec = 0,
  };
  return ts;
}

void on_timeout(ws_server_t *s, void *ctx) {
  task_t *tsk = ctx;
  size_t len = strlen(tsk->msg);

  int stat = ws_conn_send_msg(tsk->ctx, tsk->msg, len, OP_TXT, false);
  assert(stat == WS_SEND_OK);

  // task done
  free(ctx);
  tasks_completed++;
}

// this guy runs in ther server thread always
// it is to be used for workers to schedule shit that needs to
// run in the server's event loop the use of this is to write to sockets or set
// timeouts, etc it is NOT meant to be used for blocking tasks, instead it's for
// blocking tasks that are done and need to then write to a socket
void ws_server_bound_cb(ws_server_t *s, void *ctx) {
  task_t *tsk = ctx;
  size_t len = strlen(tsk->msg);

  struct timespec ts = rand_duration(25000000, 50000000);

  // this timeout is here to intentionally
  // complicate the flow for testing purposes...
  ws_server_set_timeout(s, &ts, ctx, on_timeout);
}

void handle_task(task_t *tsk) {
  size_t len = strlen(tsk->msg);
  // printf("got the task len:%zu msg:%s\n", len, tsk->msg);

  // simulate some file I/O
  int fd = open("/dev/null", O_WRONLY);
  if (fd != -1) {
    while (1) {
      ssize_t n = write(fd, tsk->msg, len);
      if (n == -1 && errno == EINTR) {
        continue;
      } else if (n == -1) {
        perror("write");
        break;
      }
      assert(n == len);
      break;
    }
    close(fd);
  }

  // simulate some more blocking work
  for (size_t i = 0; i < 1000000; i++)
    ;

  fd = open("/dev/urandom", O_RDONLY);
  if (fd != -1) {
    char buf[len];
    char ok = 0;

    while (1) {
      ssize_t n = read(fd, buf, len);
      if (n == -1 && errno == EINTR) {
        continue;
      } else if (n == -1) {
        perror("read");
        break;
      }
      assert(n == len);
      ok = 1;

      break;
    }

    if (ok) {
      memcpy(tsk->msg, buf, len);
      for (size_t i = 0; i < len; ++i)
        tsk->msg[i] = ((unsigned char)tsk->msg[i] % 57) + 65;

      tsk->msg[len - 1] = '\0';
      // printf("msg %s\n", tsk->msg);
    }

    ws_server_sched_callback(srv, ws_server_bound_cb, tsk);
  }
}

struct task_q *task_q_create() {
#if defined(__linux__)
  int fd = eventfd(0, EFD_SEMAPHORE);
#else
  int fd = kqueue();
  if (fd == -1) {
    perror("kqueue");
    return NULL;
  }
#endif
  struct task_q *tq = calloc(1, sizeof(struct task_q));
  if (tq == NULL) {
    return NULL;
  }

  if (pthread_mutex_init(&tq->mu, NULL) == -1) {
    perror("pthread_mutex_init");
    exit(EXIT_FAILURE);
  };

  tq->channel = fd;

  return tq;
}

void task_q_destroy(struct task_q *tq) {
  task_t *t = tq->task;

  while (t != NULL) {
    task_t *tmp = t;
    t = t->nxt;
    free(tmp);
  }

  pthread_mutex_destroy(&tq->mu);
  close(tq->channel);

  free(tq);
}

int task_q_add(struct task_q *tq, char *msg, size_t len, void *ctx) {
  task_t *tsk = calloc(1, sizeof(task_t) + (sizeof(char) * len));
  if (tsk == NULL) {
    perror("calloc");
    return -1;
  }

  tsk->ctx = ctx;
  if (memcpy(tsk->msg, msg, len) == NULL) {
    perror("memcpy");
    return -1;
  };

  pthread_mutex_lock(&tq->mu);
  // LIFO isn't really a queue but you get the point
  tsk->nxt = tq->task;
  tq->task = tsk;
  pthread_mutex_unlock(&tq->mu);

  // notify
  // printf("notifying\n");

#if defined(__linux__)
  eventfd_write(tq->channel, 1);
#else
  struct kevent ev[2];
  EV_SET(ev, UEVID, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  EV_SET(ev + 1, UEVID, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  if (kevent(tq->channel, ev, 2, NULL, 0, NULL) == -1) {
    perror("kevent");
  }

  UEVID++;
#endif
  return 0;
}

int task_q_add_shutdown(struct task_q *tq) {
  task_t *tsk = calloc(1, sizeof(task_t) + (sizeof(char)));
  if (tsk == NULL) {
    perror("calloc");
    return -1;
  }

  tsk->shutdown = true;

  pthread_mutex_lock(&tq->mu);
  // LIFO isn't really a queue but you get the point
  tsk->nxt = tq->task;
  tq->task = tsk;
  pthread_mutex_unlock(&tq->mu);

#if defined(__linux__)
  eventfd_write(tq->channel, 1);
#else
  // notify
  struct kevent ev[2];
  EV_SET(ev, UEVID, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  EV_SET(ev + 1, UEVID, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  if (kevent(tq->channel, ev, 2, NULL, 0, NULL) == -1) {
    perror("kevent");
  }

  UEVID++;
#endif

  return 0;
}

task_t *task_q_consume(struct task_q *tq) {
  task_t *tsk = NULL;
  pthread_mutex_lock(&tq->mu);
  if (tq->task) {
    tsk = tq->task;
    tq->task = tq->task->nxt;
  }
  pthread_mutex_unlock(&tq->mu);
  return tsk;
}

int task_q_wait(struct task_q *tq) {
#if !defined(__linux__)
  struct kevent ev;
#endif

  while (1) {
// printf("task_q_wait: waiting...\n");
#if !defined(__linux__)
    if (kevent(tq->channel, NULL, 0, &ev, 1, NULL) == -1 && errno != EINTR) {
      perror("kevent");
      return -1;
    }
#else
    eventfd_t val;
    eventfd_read(tq->channel, &val);
#endif

    task_t *tsk = task_q_consume(tq);
    if (tsk) {
      if (!tsk->shutdown) {
        handle_task(tsk);
      } else {
        // printf("worker #%zu shutting down\n", (uintptr_t)pthread_self());
        free(tsk);
        break;
      }
    } else {
      fprintf(stderr, "[WARN] task_q_wait woke up to no task\n");
    }
  }

  return 0;
}

void server_on_open(ws_conn_t *conn) {}

void server_on_msg(ws_conn_t *conn, void *msg, size_t n, uint8_t opcode) {
  // printf("got msg %s\n", (char *)msg);
  assert(task_q_add(ws_server_ctx(ws_conn_server(conn)), msg, n, conn) == 0);
}

void server_on_disconnect(ws_conn_t *conn, unsigned long err) {
  (void)conn;
  (void)err;
}

void *server_init(void *_) {
  struct ws_server_params p = {
      .addr = ADDR,
      .port = PORT,
      .on_ws_open = server_on_open,
      .on_ws_msg = server_on_msg,
      .on_ws_disconnect = server_on_disconnect,
      .max_buffered_bytes = 512,
      .max_conns = 1024,
      .silent = 1,
  };

  srv = ws_server_create(&p);

  struct task_q *tq = task_q_create();
  ws_server_set_ctx(srv, tq);

  int ret = ws_server_start(srv, 1024);

  printf("ws_server_start = %s\n", !ret ? "ok" : "failed");

  task_q_destroy(tq);
  ret = ws_server_destroy(srv);
  printf("ws_server_destroy = %s\n", !ret ? "ok" : "failed");

  return NULL;
}

void *worker_init(void *_) {
  (void)_;

  struct task_q *tq = ws_server_ctx(srv);
  assert(tq != NULL);

  int ret = task_q_wait(tq);
  assert(ret == 0);

  return NULL;
}

void *client_init(void *_) {
  (void)_;
  int fd = sock_new_connect(PORT, ADDR);
  sock_upgrade_ws(fd);

  char out_buf[128];
  char in_buf[128];

  unsigned frame_cfg = OP_TXT | 0x80;

  int msg_len = sprintf(out_buf, "hello from client #%d", fd);

  unsigned char *frame = new_frame(out_buf, msg_len, frame_cfg);
  ssize_t sent = sock_sendall(fd, frame, msg_len + 6);
  assert(sent == msg_len + 6);
  ssize_t read = sock_recvall(fd, in_buf, msg_len + 1);
  assert(read == msg_len + 1);
  free(frame);

  printf("client #%d is done\n", fd);
  return NULL;
}

int main() {
  printf("async task test starting...\n");
  pthread_t server_thr;

  // start server thread
  if (pthread_create(&server_thr, NULL, server_init, NULL) == -1) {
    perror("pthread_create");
    exit(EXIT_FAILURE);
  };

  sleep(1);

  assert(ws_server_ctx(srv) != NULL);

  // create worker threads
  pthread_t workers_thr[NUM_WORKERS];

  for (size_t i = 0; i < NUM_WORKERS; i++) {
    if (pthread_create(workers_thr + i, NULL, worker_init, NULL) == -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  printf("workers created...\n");

  pthread_t client_thr[NUM_CLIENTS];
  for (size_t i = 0; i < NUM_CLIENTS; i++) {
    if (pthread_create(client_thr + i, NULL, client_init, NULL) == -1) {
      perror("pthread_create");
      exit(EXIT_FAILURE);
    }
  }

  // clients are done first first
  for (size_t i = 0; i < NUM_CLIENTS; i++) {
    pthread_join(client_thr[i], NULL);
  }

  printf("[INFO] all clients are done\n");
  printf("[INFO] tasks completed %zu/%zu\n", tasks_completed,
         (size_t)EXPECTED_TASK_COMPLETIONS);

  assert(tasks_completed == EXPECTED_TASK_COMPLETIONS);

  for (size_t i = 0; i < NUM_WORKERS; i++) {
    assert(task_q_add_shutdown(ws_server_ctx(srv)) == 0);
  }

  for (size_t i = 0; i < NUM_WORKERS; i++) {
    pthread_join(workers_thr[i], NULL);
  }

  assert(ws_server_shutdown(srv) == 0);

  // todo: signal the end of server
  pthread_join(server_thr, NULL);

  return EXIT_SUCCESS;
}
