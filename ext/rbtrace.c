#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <ruby.h>
#include <node.h>
#include <intern.h>

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  /* return (uint64_t)tv.tv_sec*1e3 + (uint64_t)tv.tv_usec*1e-3;*/
  return (uint64_t)tv.tv_sec*1e6 + (uint64_t)tv.tv_usec;
}

#define MAX_TRACERS 100
#define MAX_CALLS 10

struct rbtracer_t {
  int id;

  char *query;
  VALUE self;
  VALUE klass;
  ID mid;
};
typedef struct rbtracer_t rbtracer_t;

static rbtracer_t tracers[MAX_TRACERS];
static unsigned int num_tracers = 0;

key_t mq_key;
int mq_id = -1;
struct event_msg {
  long mtype;
  char buf[56];
};

#define SEND_EVENT(format, ...) do {\
  uint64_t usec = timeofday_usec();\
  if (false) {\
    fprintf(stderr, "%" PRIu64 "," format, usec, __VA_ARGS__);\
    fprintf(stderr, "\n");\
  } else {\
    struct event_msg msg;\
    int ret = -1, n = 0;\
    \
    msg.mtype = 1;\
    snprintf(msg.buf, sizeof(msg.buf), "%" PRIu64 "," format, usec, __VA_ARGS__);\
    \
    for (n=0; n<10 && ret==-1; n++)\
      ret = msgsnd(mq_id, &msg, sizeof(msg)-sizeof(long), IPC_NOWAIT);\
    if (ret == -1)\
      fprintf(stderr, "msgsnd(): %s\n", strerror(errno));\
  }\
} while (0)

static int in_event_hook = 0;

static void
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  if (num_tracers == 0) return;
  if (in_event_hook) return;
  if (mid == ID_ALLOCATOR) return;
  in_event_hook++;

  int i, n;
  rbtracer_t *tracer = NULL;
  bool singleton = 0;

  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC(klass)->klass;
    }
    singleton = FL_TEST(klass, FL_SINGLETON);
  }

  for (i=0, n=0; i<MAX_TRACERS && n<num_tracers; i++) {
    if (tracers[i].query) {
      n++;

      if (tracers[i].mid == mid) {
        if (!tracers[i].klass || tracers[i].klass == klass) {
          if (!tracers[i].self || tracers[i].self == self) {
            tracer = &tracers[i];
          }
        }
      }
    }
  }
  if (!tracer) goto out;

  switch (event) {
    case RUBY_EVENT_C_CALL:
    case RUBY_EVENT_CALL:
      SEND_EVENT(
        "%s,%d,%s,%d,%s",
        event == RUBY_EVENT_CALL ? "call" : "ccall",
        tracer->id,
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );
      break;

    case RUBY_EVENT_C_RETURN:
    case RUBY_EVENT_RETURN:
      SEND_EVENT(
        "%s,%d",
        event == RUBY_EVENT_RETURN ? "return" : "creturn",
        tracer->id
      );
      break;
  }

out:
  in_event_hook--;
}

static int
rbtracer_remove(char *query, int id)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (query) {
    for (i=0; i<MAX_TRACERS; i++) {
      if (tracers[i].query) {
        if (0 == strcmp(query, tracers[i].query)) {
          tracer = &tracers[i];
          break;
        }
      }
    }
  } else {
    if (id >= MAX_TRACERS) goto out;
    tracer = &tracers[id];
  }

  if (tracer->query) {
    tracer_id = tracer->id;
    tracer->mid = 0;
    free(tracer->query);
    tracer->query = NULL;

    num_tracers--;
    if (num_tracers == 0)
      rb_remove_event_hook(event_hook);
  }

out:
  SEND_EVENT(
    "remove,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static int
rbtracer_add(char *query)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (num_tracers >= MAX_TRACERS) goto out;

  for (i=0; i<MAX_TRACERS; i++) {
    if (!tracers[i].query) {
      tracer = &tracers[i];
      tracer_id = i;
      break;
    }
  }
  if (!tracer) goto out;

  char *index;
  VALUE klass = 0, self = 0;
  ID mid;

  if (NULL != (index = rindex(query, '.'))) {
    *index = 0;
    self = rb_eval_string_protect(query, 0);
    *index = '.';

    mid = rb_intern(index+1);
  } else if (NULL != (index = rindex(query, '#'))) {
    *index = 0;
    klass = rb_eval_string_protect(query, 0);
    *index = '#';

    mid = rb_intern(index+1);
  } else {
    mid = rb_intern(query);
  }

  if (!mid) goto out;

  memset(tracer, 0, sizeof(*tracer));

  tracer->id = tracer_id;
  tracer->self = self;
  tracer->klass = klass;
  tracer->mid = mid;
  tracer->query = strdup(query);

  if (num_tracers == 0) {
    rb_add_event_hook(
      event_hook,
      RUBY_EVENT_CALL   | RUBY_EVENT_C_CALL |
      RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN
    );
  }

  num_tracers++;

out:
  SEND_EVENT(
    "add,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static VALUE
rbtrace(VALUE self, VALUE query)
{
  Check_Type(query, T_STRING);

  char *str = RSTRING(query)->ptr;
  int tracer_id = -1;

  tracer_id = rbtracer_add(str);
  return tracer_id == -1 ? Qfalse : Qtrue;
}

static VALUE
untrace(VALUE self, VALUE query)
{
  Check_Type(query, T_STRING);

  char *str = RSTRING(query)->ptr;
  int tracer_id = -1;

  tracer_id = rbtracer_remove(str, -1);
  return tracer_id == -1 ? Qfalse : Qtrue;
}

static void
cleanup()
{
  if (mq_id != -1) {
    msgctl(mq_id, IPC_RMID, NULL);
    mq_id = -1;
  }
}

static void
cleanup_ruby(VALUE data)
{
  cleanup();
}

static void
sigurg(int signal)
{
  rbtracer_add("sleep");
  return;
  if (mq_id == -1) return;

  struct event_msg msg;
  char *query = NULL;
  int len = 0;
  int ret = -1;
  int n = 0;

  for (n=0; n<10 && ret==-1; n++)
    ret = msgrcv(mq_id, &msg, sizeof(msg)-sizeof(long), 0, IPC_NOWAIT);

  if (ret == -1) {
    fprintf(stderr, "msgrcv(): %s\n", strerror(errno));
  } else {
    len = strlen(msg.buf);
    if (msg.buf[len-1] == '\n')
      msg.buf[len-1] = 0;

    printf("ohai, got: %s\n", msg.buf);
    if (0 == strcmp("add,", msg.buf)) {
      query = msg.buf + 4;
      rbtracer_add(query);
    } else if (0 == strcmp("del,", msg.buf)) {
      query = msg.buf + 4;
      rbtracer_remove(query, -1);
    }
  }
}

void
Init_rbtrace()
{
  atexit(cleanup);
  rb_set_end_proc(cleanup_ruby, 0);

  signal(SIGURG, sigurg);

  memset(&tracers, 0, sizeof(tracers));

  mq_key = (key_t) getpid();
  mq_id  = msgget(mq_key, 0666 | IPC_CREAT);

  if (mq_id == -1)
    rb_sys_fail("msgget");

  /* struct msqid_ds stat;*/
  /* msgctl(mq_id, IPC_STAT, &stat);*/
  /* printf("cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);*/

  rb_define_method(rb_cObject, "rbtrace", rbtrace, 1);
  rb_define_method(rb_cObject, "untrace", untrace, 1);
}
