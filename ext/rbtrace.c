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

#ifndef RUBY_VM
#include <env.h>
#include <intern.h>
#include <node.h>
#define rb_sourcefile() (ruby_current_node ? ruby_current_node->nd_file : 0)
#define rb_sourceline() (ruby_current_node ? nd_line(ruby_current_node) : 0)
#else
// this is a nasty hack, and will probably break on anything except 1.9.2p136
int rb_thread_method_id_and_class(void *th, ID *idp, VALUE *klassp);
RUBY_EXTERN void *ruby_current_thread;
#endif

#ifndef RSTRING_PTR
#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif
#ifndef RSTRING_LEN
#define RSTRING_LEN(str) RSTRING(str)->len
#endif

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec*1e6 + (uint64_t)tv.tv_usec;
}

#define MAX_CALLS 32768 // up to this many stack frames examined in slow watch mode
#define MAX_TRACERS 100 // max method tracers
#define MAX_EXPRS 10    // max expressions per tracer
#ifndef BUF_SIZE        // msgq buffer size
#define BUF_SIZE 120
#endif

struct rbtracer_t {
  int id;

  char *query;
  VALUE self;
  VALUE klass;
  ID mid;

  int num_exprs;
  char *exprs[MAX_EXPRS];
};
typedef struct rbtracer_t rbtracer_t;


struct event_msg {
  long mtype;
  char buf[BUF_SIZE];
};


static struct {
  bool installed;

  bool firehose;

  bool slow;
  uint64_t call_times[MAX_CALLS];
  int num_calls;
  uint32_t threshold;

  unsigned int num;
  rbtracer_t list[MAX_TRACERS];

  key_t mqo_key;
  key_t mqi_key;
  int mqo_id;
  int mqi_id;
}
rbtracer = {
  .installed = false,

  .firehose = false,

  .slow = false,
  .num_calls = 0,
  .threshold = 250,

  .num = 0,

  .mqo_key = 0,
  .mqi_key = 0,
  .mqo_id = -1,
  .mqi_id = -1
};

#define SEND_EVENT(format, ...) do {\
  uint64_t usec = timeofday_usec();\
  if (false) {\
    fprintf(stderr, "%" PRIu64 "," format, usec, __VA_ARGS__);\
    fprintf(stderr, "\n");\
  } else if (rbtracer.mqo_id != -1) {\
    struct event_msg msg;\
    int ret = -1, n = 0;\
    \
    msg.mtype = 1;\
    snprintf(msg.buf, sizeof(msg.buf), "%" PRIu64 "," format, usec, __VA_ARGS__);\
    \
    for (n=0; n<10 && ret==-1; n++)\
      ret = msgsnd(rbtracer.mqo_id, &msg, sizeof(msg)-sizeof(long), IPC_NOWAIT);\
    if (ret == -1) {\
      fprintf(stderr, "msgsnd(): %s\n", strerror(errno));\
      struct msqid_ds stat;\
      msgctl(rbtracer.mqo_id, IPC_STAT, &stat);\
      fprintf(stderr, "cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);\
    }\
  }\
} while (0)

static void
#ifdef RUBY_VM
event_hook(rb_event_flag_t event, VALUE data, VALUE self, ID mid, VALUE klass)
#else
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
#endif
{
  static int in_event_hook = 0;

  // do not re-enter this function
  // after this, must `goto out` instead of `return`
  if (in_event_hook) return;
  in_event_hook++;

  // skip allocators
  if (mid == ID_ALLOCATOR) goto out;

  // some serious 1.9.2 hax
#ifdef RUBY_VM
  if (mid == 0 && ruby_current_thread) {
    ID _mid;
    VALUE _klass;
    rb_thread_method_id_and_class(ruby_current_thread, &_mid, &_klass);

    mid = _mid;
    klass = _klass;
  }
#endif

  // normalize klass and check for class-level methods
  bool singleton = 0;
  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC(klass)->klass;
    }
    singleton = FL_TEST(klass, FL_SINGLETON);
  }

  // are we watching for any slow methods?
  if (rbtracer.slow) {
    uint64_t usec = timeofday_usec(), diff = 0;

    switch (event) {
      case RUBY_EVENT_C_CALL:
      case RUBY_EVENT_CALL:
        if (rbtracer.num_calls < MAX_CALLS)
          rbtracer.call_times[ rbtracer.num_calls ] = usec;

        rbtracer.num_calls++;
        break;

      case RUBY_EVENT_C_RETURN:
      case RUBY_EVENT_RETURN:
        if (rbtracer.num_calls > 0) {
          rbtracer.num_calls--;

          if (rbtracer.num_calls < MAX_CALLS)
            diff = usec - rbtracer.call_times[ rbtracer.num_calls ];
        }
        break;
    }

    if (diff > rbtracer.threshold * 1e3) {
      SEND_EVENT(
        "%s,-1,%" PRIu64 ",%d,%s,%d,%s",
        event == RUBY_EVENT_RETURN ? "slow" : "cslow",
        diff,
        rbtracer.num_calls,
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );
    }

    goto out;
  }

  int i, n;
  rbtracer_t *tracer = NULL;

  if (!rbtracer.firehose) {
    // are there specific methods we're waiting for?
    if (rbtracer.num == 0) goto out;

    for (i=0, n=0; i<MAX_TRACERS && n<rbtracer.num; i++) {
      if (rbtracer.list[i].query) {
        n++;

        if (!rbtracer.list[i].mid || rbtracer.list[i].mid == mid) {
          if (!rbtracer.list[i].klass || rbtracer.list[i].klass == klass) {
            if (!rbtracer.list[i].self || rbtracer.list[i].self == self) {
              tracer = &rbtracer.list[i];
            }
          }
        }
      }
    }

    // no matching method tracer found, so bail!
    if (!tracer) goto out;
  }

  switch (event) {
    case RUBY_EVENT_CALL:
    case RUBY_EVENT_C_CALL:
      SEND_EVENT(
        "%s,%d,%s,%d,%s",
        event == RUBY_EVENT_CALL ? "call" : "ccall",
        tracer ? tracer->id : 255, // hax
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );

      if (tracer && tracer->num_exprs) {
        for (i=0; i<tracer->num_exprs; i++) {
          char *expr = tracer->exprs[i];
          size_t len = strlen(expr);

          VALUE val = Qnil;
          char buffer[len+150];
          char *result = NULL;

          if (len == 4 && strcmp("self", expr) == 0) {
            val = rb_inspect(self);

          } else if (len == 10 && strcmp("__source__", expr) == 0) {
            snprintf(buffer, len+150, "\"%s:%d\"", rb_sourcefile(), rb_sourceline());
            result = buffer;

          } else if (len > 1 && expr[0] == '@') {
            val = rb_inspect(rb_ivar_get(self, rb_intern(expr)));

          } else if (event == RUBY_EVENT_CALL) {
            snprintf(buffer, len+150, "(begin; ObjectSpace._id2ref(%p >> 1).instance_eval{ %s }; rescue Exception => e; e; end).inspect", (void*)self, expr);
            val = rb_eval_string_protect(buffer, 0);
          }

          if (RTEST(val) && TYPE(val) == T_STRING) {
            result = RSTRING_PTR(val);
          }

          if (result) {
            SEND_EVENT(
              "%s,%d,%d,%s",
              "exprval",
              tracer->id,
              i,
              result
            );
          }
        }
      }
      break;

    case RUBY_EVENT_RETURN:
    case RUBY_EVENT_C_RETURN:
      SEND_EVENT(
        "%s,%d",
        event == RUBY_EVENT_RETURN ? "return" : "creturn",
        tracer ? tracer->id : 255 // hax
      );
      break;
  }

out:
  in_event_hook--;
}

static void
event_hook_install()
{
  if (!rbtracer.installed) {
    rb_add_event_hook(
      event_hook,
      RUBY_EVENT_CALL   | RUBY_EVENT_C_CALL |
      RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN
#ifdef RB_EVENT_HOOKS_HAVE_CALLBACK_DATA
      , 0
#endif
    );
    rbtracer.installed = true;
  }
}

static void
event_hook_remove()
{
  if (rbtracer.installed) {
    rb_remove_event_hook(event_hook);
    rbtracer.installed = false;
  }
}

static int
rbtracer_remove(char *query, int id)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (query) {
    for (i=0; i<MAX_TRACERS; i++) {
      if (rbtracer.list[i].query) {
        if (0 == strcmp(query, rbtracer.list[i].query)) {
          tracer = &rbtracer.list[i];
          break;
        }
      }
    }
  } else {
    if (id >= MAX_TRACERS) goto out;
    tracer = &rbtracer.list[id];
  }

  if (tracer->query) {
    tracer_id = tracer->id;
    tracer->mid = 0;

    free(tracer->query);
    tracer->query = NULL;

    if (tracer->num_exprs) {
      for(i=0; i<tracer->num_exprs; i++) {
        free(tracer->exprs[i]);
        tracer->exprs[i] = NULL;
      }
      tracer->num_exprs = 0;
    }

    rbtracer.num--;
    if (rbtracer.num == 0)
      event_hook_remove();
  }

out:
  SEND_EVENT(
    "remove,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static void
rbtracer_remove_all()
{
  rbtracer.firehose = false;
  rbtracer.slow = false;

  int i;
  for (i=0; i<MAX_TRACERS; i++) {
    if (rbtracer.list[i].query) {
      rbtracer_remove(NULL, i);
    }
  }
}

static int
rbtracer_add(char *query)
{
  int i;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (rbtracer.num >= MAX_TRACERS) goto out;

  for (i=0; i<MAX_TRACERS; i++) {
    if (!rbtracer.list[i].query) {
      tracer = &rbtracer.list[i];
      tracer_id = i;
      break;
    }
  }
  if (!tracer) goto out;

  char *idx, *method;
  VALUE klass = 0, self = 0;
  ID mid = 0;

  if (NULL != (idx = rindex(query, '.'))) {
    *idx = 0;
    self = rb_eval_string_protect(query, 0);
    *idx = '.';

    method = idx+1;
  } else if (NULL != (idx = rindex(query, '#'))) {
    *idx = 0;
    klass = rb_eval_string_protect(query, 0);
    *idx = '#';

    method = idx+1;
  } else {
    method = query;
  }

  if (method && *method) {
    mid = rb_intern(method);
    if (!mid) goto out;
  } else if (klass || self) {
    mid = 0;
  } else {
    goto out;
  }

  memset(tracer, 0, sizeof(*tracer));

  tracer->id = tracer_id;
  tracer->self = self;
  tracer->klass = klass;
  tracer->mid = mid;
  tracer->query = strdup(query);
  tracer->num_exprs = 0;

  if (rbtracer.num == 0)
    event_hook_install();

  rbtracer.num++;

out:
  SEND_EVENT(
    "add,%d,%s",
    tracer_id,
    query
  );
  return tracer_id;
}

static void
rbtracer_add_expr(int id, char *expr)
{
  int expr_id = -1;
  int tracer_id = -1;
  rbtracer_t *tracer = NULL;

  if (id >= MAX_TRACERS) goto out;
  tracer = &rbtracer.list[id];

  if (tracer->query) {
    tracer_id = tracer->id;

    if (tracer->num_exprs < MAX_EXPRS) {
      expr_id = tracer->num_exprs++;
      tracer->exprs[expr_id] = strdup(expr);
    }
  }

out:
  SEND_EVENT(
    "newexpr,%d,%d,%s",
    tracer_id,
    expr_id,
    expr
  );
}

static void
rbtracer_watch(uint32_t threshold)
{
  if (!rbtracer.slow) {
    rbtracer.num_calls = 0;
    rbtracer.threshold = threshold;
    rbtracer.firehose = false;
    rbtracer.slow = true;

    event_hook_install();
  }
}

static void
rbtracer_unwatch()
{
  if (rbtracer.slow) {
    event_hook_remove();

    rbtracer.firehose = false;
    rbtracer.slow = false;
  }
}

static void
msgq_teardown()
{
  if (rbtracer.mqo_id != -1) {
    msgctl(rbtracer.mqo_id, IPC_RMID, NULL);
    rbtracer.mqo_id = -1;
    rbtracer.mqo_key = 0;
  }
  if (rbtracer.mqi_id != -1) {
    msgctl(rbtracer.mqi_id, IPC_RMID, NULL);
    rbtracer.mqi_id = -1;
    rbtracer.mqi_key = 0;
  }
}

static void
ruby_teardown(VALUE data)
{
  msgq_teardown();
}

static void
msgq_setup()
{
  pid_t pid = getpid();

  if (rbtracer.mqo_key != (key_t)pid ||
      rbtracer.mqi_key != (key_t)-pid) {
    msgq_teardown();
  } else {
    return;
  }

  rbtracer.mqo_key = (key_t) pid;
  rbtracer.mqo_id  = msgget(rbtracer.mqo_key, 0666 | IPC_CREAT);

  if (rbtracer.mqo_id == -1)
    fprintf(stderr, "msgget() failed to create msgq\n");


  rbtracer.mqi_key = (key_t) -pid;
  rbtracer.mqi_id  = msgget(rbtracer.mqi_key, 0666 | IPC_CREAT);

  if (rbtracer.mqi_id == -1)
    fprintf(stderr, "msgget() failed to create msgq\n");

  /*
  struct msqid_ds stat;
  int ret;

  msgctl(rbtracer.mqo_id, IPC_STAT, &stat);
  printf("cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);

  stat.msg_qbytes += 10;
  ret = msgctl(rbtracer.mqo_id, IPC_SET, &stat);
  printf("cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);
  printf("ret: %d, errno: %d\n", ret, errno);

  msgctl(rbtracer.mqo_id, IPC_STAT, &stat);
  printf("cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);
  */
}

static void
sigurg(int signal)
{
  static int last_tracer_id = -1; // hax
  msgq_setup();
  if (rbtracer.mqi_id == -1) return;

  struct event_msg msg;
  char *query = NULL;
  size_t len = 0;
  int n = 0;

  while (true) {
    int ret = -1;

    for (n=0; n<10 && ret==-1; n++)
      ret = msgrcv(rbtracer.mqi_id, &msg, sizeof(msg)-sizeof(long), 0, IPC_NOWAIT);

    if (ret == -1) {
      break;
    } else {
      len = strlen(msg.buf);
      if (msg.buf[len-1] == '\n')
        msg.buf[len-1] = 0;

      if (0 == strncmp("add,", msg.buf, 4)) {
        query = msg.buf + 4;
        last_tracer_id = rbtracer_add(query);

      } else if (0 == strncmp("del,", msg.buf, 4)) {
        query = msg.buf + 4;
        rbtracer_remove(query, -1);

      } else if (0 == strncmp("firehose", msg.buf, 8)) {
        rbtracer.firehose = true;
        event_hook_install();

      } else if (0 == strncmp("addexpr,", msg.buf, 8)) {
        query = msg.buf + 8;
        rbtracer_add_expr(last_tracer_id, query);

      } else if (0 == strncmp("watch,", msg.buf, 6)) {
        int msec = 250;

        query = msg.buf + 6;
        if (query && *query)
          msec = atoi(query);

        rbtracer_watch(msec);

      } else if (0 == strncmp("unwatch", msg.buf, 7)) {
        rbtracer_unwatch();

      } else if (0 == strncmp("detach", msg.buf, 6)) {
        rbtracer_remove_all();

      }
    }
  }
}

void
Init_rbtrace()
{
  // zero out tracer
  memset(&rbtracer.list, 0, sizeof(rbtracer.list));

  // catch signal telling us to read from the msgq
  signal(SIGURG, sigurg);

  // cleanup the msgq on exit
  atexit(msgq_teardown);
  rb_set_end_proc(ruby_teardown, 0);
}
