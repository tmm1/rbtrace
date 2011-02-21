#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
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

#include <msgpack.h>
#include <ruby.h>

#ifndef RUBY_VM
#include <env.h>
#include <intern.h>
#include <node.h>
#include <st.h>
#define rb_sourcefile() (ruby_current_node ? ruby_current_node->nd_file : 0)
#define rb_sourceline() (ruby_current_node ? nd_line(ruby_current_node) : 0)
#else
#include <ruby/st.h>
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

typedef struct {
  int id;

  char *query;
  VALUE self;
  VALUE klass;
  ID mid;

  int num_exprs;
  char *exprs[MAX_EXPRS];
} rbtracer_t;

typedef struct {
  long mtype;
  char buf[BUF_SIZE];
} event_msg_t;

static struct {
  st_table *mid_tbl;
  st_table *klass_tbl;

  pid_t attached_pid;

  bool installed;

  bool gc;
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

  msgpack_sbuffer *sbuf;
  msgpack_packer *msgpacker;
}
rbtracer = {
  .mid_tbl = NULL,
  .klass_tbl = NULL,

  .attached_pid = 0,

  .installed = false,

  .gc = false,
  .firehose = false,

  .slow = false,
  .num_calls = 0,
  .threshold = 250,

  .num = 0,
  .list = {},

  .mqo_key = 0,
  .mqi_key = 0,
  .mqo_id = -1,
  .mqi_id = -1,

  .sbuf = NULL,
  .msgpacker = NULL
};

static inline void
rbtrace__send_event(int nargs, const char *name, ...)
{
  if (!rbtracer.attached_pid ||
      !rbtracer.sbuf ||
      !rbtracer.msgpacker ||
      rbtracer.mqo_id == -1)
    return;

  int n;

  msgpack_sbuffer_clear(rbtracer.sbuf);
  msgpack_packer *pk = rbtracer.msgpacker;

  msgpack_pack_array(pk, nargs+1);

  msgpack_pack_raw(pk, strlen(name));
  msgpack_pack_raw_body(pk, name, strlen(name));

  if (nargs > 0) {
    int type;

    int sint;
    uint32_t uint;
    uint64_t uint64;
    unsigned long ulong;
    char *str;

    va_list ap;
    va_start(ap, name);

    for (n=0; n<nargs; n++) {
      type = va_arg(ap, int);
      switch (type) {
        case 't':
          uint64 = va_arg(ap, uint64_t);
          msgpack_pack_uint64(pk, uint64);
          break;

        case 'n':
          msgpack_pack_uint64(pk, timeofday_usec());
          break;

        case 'b':
          if (va_arg(ap, int))
            msgpack_pack_true(pk);
          else
            msgpack_pack_false(pk);
          break;

        case 'u':
          uint = va_arg(ap, uint32_t);
          msgpack_pack_uint32(pk, uint);
          break;

        case 'l':
          ulong = va_arg(ap, unsigned long);
          msgpack_pack_unsigned_long(pk, ulong);
          break;

        case 'd':
          sint = va_arg(ap, int);
          msgpack_pack_int(pk, sint);
          break;

        case 's':
          str = va_arg(ap, char *);
          if (!str)
            str = (char *)"";

          msgpack_pack_raw(pk, strlen(str));
          msgpack_pack_raw_body(pk, str, strlen(str));
          break;

        default:
          fprintf(stderr, "unknown type (%c) passed to rbtrace__send_event\n", (char)type);
      }
    }

    va_end(ap);
  }

  event_msg_t msg;
  msg.mtype = 1;

  if (rbtracer.sbuf->size > sizeof(msg.buf)) {
    fprintf(stderr, "rbtrace__send_event(): message is too large (%zd > %lu)\n", rbtracer.sbuf->size, sizeof(msg.buf));
    return;
  }

  memcpy(msg.buf, rbtracer.sbuf->data, rbtracer.sbuf->size);

  int ret = -1;
  for (n=0; n<10 && ret==-1; n++)
    ret = msgsnd(rbtracer.mqo_id, &msg, sizeof(msg)-sizeof(long), IPC_NOWAIT);

  if (ret == -1 && rbtracer.mqo_id != -1 && errno != EINVAL) {
    fprintf(stderr, "msgsnd(%d): %s\n", rbtracer.mqo_id, strerror(errno));

    struct msqid_ds stat;
    msgctl(rbtracer.mqo_id, IPC_STAT, &stat);
    fprintf(stderr, "cbytes: %lu, qbytes: %lu, qnum: %lu\n", stat.msg_cbytes, stat.msg_qbytes, stat.msg_qnum);
  }
}

static inline void
rbtrace__send_names(ID mid, VALUE klass)
{
  if (!rbtracer.mid_tbl)
    rbtracer.mid_tbl = st_init_numtable();

  if (!st_is_member(rbtracer.mid_tbl, mid)) {
    st_insert(rbtracer.mid_tbl, (st_data_t)mid, (st_data_t)1);
    rbtrace__send_event(2,
      "mid",
      'l', mid,
      's', rb_id2name(mid)
    );
  }

  if (!rbtracer.klass_tbl)
    rbtracer.klass_tbl = st_init_numtable();

  if (!st_is_member(rbtracer.klass_tbl, klass)) {
    st_insert(rbtracer.klass_tbl, (st_data_t)klass, (st_data_t)1);
    rbtrace__send_event(2,
      "klass",
      'l', klass,
      's', rb_class2name(klass)
    );
  }
}

static void
rbtracer__resolve_query(char *query, VALUE *klass, VALUE *self, ID *mid)
{
  char *idx = NULL, *method = NULL;

  assert(klass && self && mid);
  *klass = *self = *mid = 0;

  if (NULL != (idx = rindex(query, '.'))) {
    *idx = 0;
    *self = rb_eval_string_protect(query, 0);
    *idx = '.';

    method = idx+1;
  } else if (NULL != (idx = rindex(query, '#'))) {
    *idx = 0;
    *klass = rb_eval_string_protect(query, 0);
    *idx = '#';

    method = idx+1;
  } else {
    method = query;
  }

  if (method && *method) {
    *mid = rb_intern(method);
  }
}

static int in_event_hook = 0;

static void
#ifdef RUBY_VM
event_hook(rb_event_flag_t event, VALUE data, VALUE self, ID mid, VALUE klass)
#else
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
#endif
{
  // do not re-enter this function
  // after this, must `goto out` instead of `return`
  if (in_event_hook) return;
  in_event_hook++;

  // skip allocators
  if (mid == ID_ALLOCATOR) goto out;

#ifdef RUBY_VM
  // some serious 1.9.2 hax
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

  rbtracer_t *tracer = NULL;

  if (rbtracer.firehose) {
    // trace everything

  } else if (rbtracer.num > 0) {
    // tracing only specific methods
    int i, n;
    for (i=0, n=0; i<MAX_TRACERS && n<rbtracer.num; i++) {
      rbtracer_t *curr = &rbtracer.list[i];

      if (curr->query) {
        n++;

        if ((!curr->mid   || curr->mid == mid) &&
            (!curr->klass || curr->klass == klass) &&
            (!curr->self  || curr->self == self))
        {
          tracer = curr;
          break;
        }
      }
    }

    // no tracer for current method call
    if (!tracer) goto out;

  } else if (rbtracer.slow) {
    // trace anything that's slow
    // fall through to slow logic below, after the previous conditional
    // selects specific methods we might be interested in

  } else {
    // what are we doing here?
    goto out;
  }

  // are we watching for slow method calls?
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
      rbtrace__send_names(mid, singleton ? self : klass);
      rbtrace__send_event(6,
        event == RUBY_EVENT_RETURN ? "slow" : "cslow",
        't', rbtracer.call_times[ rbtracer.num_calls ],
        'u', diff,
        'u', rbtracer.num_calls,
        'l', mid,
        'b', singleton,
        'l', singleton ? self : klass
      );
    }

    goto out;
  }

  switch (event) {
    case RUBY_EVENT_CALL:
    case RUBY_EVENT_C_CALL:
      rbtrace__send_names(mid, singleton ? self : klass);
      rbtrace__send_event(5,
        event == RUBY_EVENT_CALL ? "call" : "ccall",
        'n',
        'd', tracer ? tracer->id : 255, // hax
        'l', mid,
        'b', singleton,
        'l', singleton ? self : klass
      );

      if (tracer && tracer->num_exprs) {
        int i;
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

          } else if (len > 2 && expr[0] == '@' && expr[1] != '@') {
            val = rb_inspect(rb_ivar_get(self, rb_intern(expr)));

          } else {
            snprintf(buffer, len+150, "(begin; ObjectSpace._id2ref(%p >> 1).instance_eval{ %s }; rescue Exception => e; e; end).inspect", (void*)self, expr);
            val = rb_eval_string_protect(buffer, 0);
          }

          if (RTEST(val) && TYPE(val) == T_STRING) {
            result = RSTRING_PTR(val);
          }

          if (result && *result) {
            rbtrace__send_event(3,
              "exprval",
              'd', tracer->id,
              'd', i,
              's', result
            );
          }
        }
      }
      break;

    case RUBY_EVENT_RETURN:
    case RUBY_EVENT_C_RETURN:
      rbtrace__send_event(2,
        event == RUBY_EVENT_RETURN ? "return" : "creturn",
        'n',
        'd', tracer ? tracer->id : 255 // hax
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
  rbtrace__send_event(2,
    "remove",
    'd', tracer_id,
    's', query
  );
  return tracer_id;
}

static void
rbtracer_remove_all()
{
  rbtracer.firehose = false;
  rbtracer.slow = false;
  rbtracer.gc = false;

  int i;
  for (i=0; i<MAX_TRACERS; i++) {
    if (rbtracer.list[i].query) {
      rbtracer_remove(NULL, i);
    }
  }

  if (rbtracer.mid_tbl)
    st_free_table(rbtracer.mid_tbl);
  rbtracer.mid_tbl = NULL;

  if (rbtracer.klass_tbl)
    st_free_table(rbtracer.klass_tbl);
  rbtracer.klass_tbl = NULL;
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

  VALUE klass = 0, self = 0;
  ID mid = 0;

  rbtracer__resolve_query(query, &klass, &self, &mid);

  if (!mid && !klass && !self)
    goto out;

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
  rbtrace__send_event(2,
    "add",
    'd', tracer_id,
    's', query
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
  rbtrace__send_event(3,
    "newexpr",
    'd', tracer_id,
    'd', expr_id,
    's', expr
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

static inline void
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
}

static void
sigurg(int signal)
{
  static int last_tracer_id = -1; // hax
  msgq_setup();
  if (rbtracer.mqi_id == -1) return;

  event_msg_t msg;
  int n = 0;

  while (true) {
    int ret = -1;

    for (n=0; n<10 && ret==-1; n++)
      ret = msgrcv(rbtracer.mqi_id, &msg, sizeof(msg)-sizeof(long), 0, IPC_NOWAIT);

    if (ret == -1) {
      break;
    } else {
      char query[sizeof(msg.buf)];

      msgpack_object cmd;
      msgpack_object_array ary;
      msgpack_object_raw str;

      msgpack_unpacked unpacked;
      msgpack_unpacked_init(&unpacked);

      bool success = msgpack_unpack_next(&unpacked, msg.buf, sizeof(msg.buf), NULL);
      cmd = unpacked.data;

      if (!success || cmd.type != MSGPACK_OBJECT_ARRAY)
        continue;

      /* fprintf(stderr, "GOT: ");*/
      /* msgpack_object_print(stderr, cmd);*/
      /* fprintf(stderr, "\n");*/

      ary = cmd.via.array;

      if (ary.size < 1 ||
          ary.ptr[0].type != MSGPACK_OBJECT_RAW)
        continue;

      str = ary.ptr[0].via.raw;

      if (0 == strncmp("attach", str.ptr, str.size)) {
        if (ary.size != 2 ||
            ary.ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
          continue;

        pid_t pid = (pid_t) ary.ptr[1].via.u64;

        if (pid && rbtracer.attached_pid == 0)
          rbtracer.attached_pid = pid;

        rbtrace__send_event(1,
          "attached",
          'u', (uint32_t) rbtracer.attached_pid
        );

      } else if (0 == strncmp("detach", str.ptr, str.size)) {
        if (rbtracer.attached_pid) {
          rbtrace__send_event(1,
            "detached",
            'u', (uint32_t) rbtracer.attached_pid
          );

          rbtracer.attached_pid = 0;
          rbtracer_remove_all();
        }

      } else if (0 == strncmp("watch", str.ptr, str.size)) {
        if (ary.size != 2 ||
            ary.ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
          continue;

        unsigned int msec = ary.ptr[1].via.u64;
        rbtracer_watch(msec);

      } else if (0 == strncmp("firehose", str.ptr, str.size)) {
        rbtracer.firehose = true;
        event_hook_install();

      } else if (0 == strncmp("add", str.ptr, str.size)) {
        if (ary.size != 2 ||
            ary.ptr[1].type != MSGPACK_OBJECT_RAW)
          continue;

        str = ary.ptr[1].via.raw;

        strncpy(query, str.ptr, str.size);
        query[str.size] = 0;
        last_tracer_id = rbtracer_add(query);

      } else if (0 == strncmp("addexpr", str.ptr, str.size)) {
        if (ary.size != 2 ||
            ary.ptr[1].type != MSGPACK_OBJECT_RAW)
          continue;

        str = ary.ptr[1].via.raw;

        strncpy(query, str.ptr, str.size);
        query[str.size] = 0;
        rbtracer_add_expr(last_tracer_id, query);

      } else if (0 == strncmp("gc", str.ptr, str.size)) {
        rbtracer.gc = true;

      }
    }
  }
}

static void
rbtrace_gc_mark()
{
  if (rbtracer.gc && !in_event_hook) {
    rbtrace__send_event(1,
      "gc",
      'n'
    );
  }
}

static VALUE gc_hook;

void
Init_rbtrace()
{
  // hook into the gc
  gc_hook = Data_Wrap_Struct(rb_cObject, rbtrace_gc_mark, NULL, NULL);
  rb_global_variable(&gc_hook);

  // setup msgpack
  rbtracer.sbuf = msgpack_sbuffer_new();
  rbtracer.msgpacker = msgpack_packer_new(rbtracer.sbuf, msgpack_sbuffer_write);

  // zero out tracer
  memset(&rbtracer.list, 0, sizeof(rbtracer.list));

  // catch signal telling us to read from the msgq
  signal(SIGURG, sigurg);

  // cleanup the msgq on exit
  atexit(msgq_teardown);
  rb_set_end_proc(ruby_teardown, 0);
}
