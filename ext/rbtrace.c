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
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <msgpack.h>
#include <ruby.h>

#ifndef RUBY_VM
#include <env.h>
#include <node.h>
#include <st.h>
#define rb_sourcefile() (ruby_current_node ? ruby_current_node->nd_file : 0)
#define rb_sourceline() (ruby_current_node ? nd_line(ruby_current_node) : 0)
#else
#include <ruby/st.h>
#endif

#ifndef RSTRING_PTR
#define RSTRING_PTR(str) RSTRING(str)->ptr
#endif
#ifndef RSTRING_LEN
#define RSTRING_LEN(str) RSTRING(str)->len
#endif
#ifndef RBASIC_CLASS
#define RBASIC_CLASS(obj) (RBASIC(obj)->klass)
#endif


#ifdef __FreeBSD__
 #define PLATFORM_FREEBSD
#endif

#ifdef __linux__
 #define PLATFORM_LINUX
#endif


static uint64_t
ru_utime_usec()
{
  struct rusage r_usage;
  getrusage(RUSAGE_SELF, &r_usage);
  return (uint64_t)r_usage.ru_utime.tv_sec*1e6 +
         (uint64_t)r_usage.ru_utime.tv_usec;
}

static uint64_t
timeofday_usec()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec*1e6 +
         (uint64_t)tv.tv_usec;
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
  bool is_slow;

  char *klass_name;
  size_t klass_len;
  bool is_singleton;

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
  bool devmode;

  bool gc;
  bool firehose;

  bool slow;
  bool slowcpu;
  uint64_t call_times[MAX_CALLS];
  uint64_t call_utimes[MAX_CALLS];
  int num_calls;
  uint32_t threshold;

  unsigned int num;
  unsigned int num_slow;
  rbtracer_t list[MAX_TRACERS];

  key_t mqi_key;
  int mqi_id;

  int mqo_fd;
  struct sockaddr_un mqo_addr;
  socklen_t mqo_len;

  msgpack_sbuffer *sbuf;
  msgpack_packer *msgpacker;
}
rbtracer = {
  .mid_tbl = NULL,
  .klass_tbl = NULL,

  .attached_pid = 0,

  .installed = false,
  .devmode = false,

  .gc = false,
  .firehose = false,

  .slow = false,
  .slowcpu = false,
  .num_calls = 0,
  .threshold = 250,

  .num = 0,
  .num_slow = 0,
  .list = {},

  .mqi_key = 0,
  .mqi_id = -1,

  .mqo_fd = -1,
  .mqo_addr = {.sun_family = AF_UNIX},

  .sbuf = NULL,
  .msgpacker = NULL
};

static void
  msgq_teardown(),
  rbtracer_detach();

static inline void
rbtrace__send_event(int nargs, const char *name, ...)
{
  if (!rbtracer.attached_pid ||
      !rbtracer.sbuf ||
      !rbtracer.msgpacker ||
      rbtracer.mqo_fd == -1)
    return;

  int n;

  msgpack_sbuffer_clear(rbtracer.sbuf);
  msgpack_packer *pk = rbtracer.msgpacker;

  msgpack_pack_array(pk, nargs+1);

  msgpack_pack_bin(pk, strlen(name));
  msgpack_pack_bin_body(pk, name, strlen(name));

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
        case 'b': // boolean
          if (va_arg(ap, int))
            msgpack_pack_true(pk);
          else
            msgpack_pack_false(pk);
          break;

        case 'd': // signed int
          sint = va_arg(ap, int);
          msgpack_pack_int(pk, sint);
          break;

        case 'u': // unsigned int
          uint = va_arg(ap, uint32_t);
          msgpack_pack_uint32(pk, uint);
          break;

        case 'l': // unsigned long (VALUE/ID)
          ulong = va_arg(ap, unsigned long);
          msgpack_pack_unsigned_long(pk, ulong);
          break;

        case 't': // uint64 (timestamps)
          uint64 = va_arg(ap, uint64_t);
          msgpack_pack_uint64(pk, uint64);
          break;

        case 'n': // current timestamp
          msgpack_pack_uint64(pk, timeofday_usec());
          break;

        case 's': // string
          str = va_arg(ap, char *);
          if (!str)
            str = (char *)"";

          msgpack_pack_bin(pk, strlen(str));
          msgpack_pack_bin_body(pk, str, strlen(str));
          break;

        default:
          fprintf(stderr, "unknown type (%d) passed to rbtrace__send_event for %s\n", (int)type, name);
      }
    }

    va_end(ap);
  }

  int ret = -1;
  for (n=0; n<10 && ret==-1; n++)
    ret = sendto(
      rbtracer.mqo_fd,
      rbtracer.sbuf->data, rbtracer.sbuf->size,
#ifdef MSG_NOSIGNAL
      MSG_NOSIGNAL,
#else
      0,
#endif
      (const struct sockaddr *)&rbtracer.mqo_addr, rbtracer.mqo_len
    );

  if (ret == -1 && (errno == EINVAL || errno == ENOENT || errno == ECONNREFUSED || errno == EPIPE)) {
    fprintf(stderr, "sendto(%d): %s [detaching]\n", rbtracer.mqo_fd, strerror(errno));

    msgq_teardown();
    rbtracer_detach();
  } else if (ret == -1) {
    fprintf(stderr, "sendto(%d): %s\n", rbtracer.mqo_fd, strerror(errno));
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

  if (rbtracer.devmode || !st_is_member(rbtracer.klass_tbl, klass)) {
    if (!rbtracer.devmode)
      st_insert(rbtracer.klass_tbl, (st_data_t)klass, (st_data_t)1);

    rbtrace__send_event(2,
      "klass",
      'l', klass,
      's', rb_class2name(klass)
    );
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

#ifdef ID_ALLOCATOR
  // skip allocators
  if (mid == ID_ALLOCATOR) goto out;
#endif

#ifdef RUBY_VM
  if (mid == 0) {
    ID _mid;
    VALUE _klass;
    rb_frame_method_id_and_class(&_mid, &_klass);

    mid = _mid;
    klass = _klass;
  }
#endif

  // normalize klass and check for class-level methods
  bool singleton = false;
  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC_CLASS(klass);
    }

    singleton = FL_TEST(klass, FL_SINGLETON);

#ifdef RUBY_VM
    if (singleton &&
        !(TYPE(self) == T_CLASS ||
          TYPE(self) == T_MODULE))
      singleton = false;
#endif
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

        // there should never be slow method tracers outside slow mode
        if (!rbtracer.slow && curr->is_slow) continue;

        if (rbtracer.devmode) {
          if ((!curr->mid        || curr->mid == mid) &&
              (!curr->klass_name || (
                (singleton == curr->is_singleton) &&
                (0 == strncmp(rb_class2name(singleton ? self : klass), curr->klass_name, curr->klass_len)))))
          {
            tracer = curr;
            break;
          }
        } else {
          if ((!curr->mid   || curr->mid == mid) &&
              (!curr->klass || curr->klass == klass) &&
              (!curr->self  || curr->self == self))
          {
            tracer = curr;
            break;
          }
        }
      }
    }

    if (tracer) {
      // matched something, all good!
    } else if (rbtracer.slow && rbtracer.num_slow == 0) {
      // in global slow mode, so go ahead.
    } else {
      goto out;
    }

  } else if (rbtracer.slow && rbtracer.num_slow == 0) {
    // trace everything that's slow

  } else {
    // what are we doing here?
    goto out;
  }

  // are we watching for slow method calls?
  if (rbtracer.slow && (!tracer || tracer->is_slow)) {
    uint64_t usec = timeofday_usec(),
             ut_usec = ru_utime_usec(),
             diff = 0;

    switch (event) {
      case RUBY_EVENT_C_CALL:
      case RUBY_EVENT_CALL:
        if (rbtracer.num_calls < MAX_CALLS) {
          rbtracer.call_times[ rbtracer.num_calls ] = usec;
          if (rbtracer.slowcpu)
            rbtracer.call_utimes[ rbtracer.num_calls ] = ut_usec;
        }

        rbtracer.num_calls++;
        break;

      case RUBY_EVENT_C_RETURN:
      case RUBY_EVENT_RETURN:
        if (rbtracer.num_calls > 0) {
          rbtracer.num_calls--;

          if (rbtracer.num_calls < MAX_CALLS) {
            if (rbtracer.slowcpu)
              diff = ut_usec - rbtracer.call_utimes[ rbtracer.num_calls ];
            else
              diff = usec - rbtracer.call_times[ rbtracer.num_calls ];
          }
        }
        break;
    }

    if (diff > rbtracer.threshold * 1e3) {
      rbtrace__send_names(mid, singleton ? self : klass);
      rbtrace__send_event(6,
        event == RUBY_EVENT_RETURN ? "slow" : "cslow",
        't', rbtracer.call_times[ rbtracer.num_calls ],
        't', diff,
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
            snprintf(buffer, len+150, "(begin; ObjectSpace._id2ref(%ld).instance_eval{ %s }; rescue Exception => e; e; end).inspect", NUM2LONG(rb_obj_id(self)), expr);
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
#ifdef RUBY_VM
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

#ifdef HAVE_RB_GC_ADD_EVENT_HOOK
// requires https://github.com/tmm1/brew2deb/blob/master/packages/ruby/patches/gc-hooks.patch
static void
rbtrace_gc_event_hook(rb_gc_event_t gc_event, VALUE obj)
{
  switch(gc_event)
  {
    case RUBY_GC_EVENT_START:
      rbtrace__send_event(1,
        "gc_start",
        'n'
      );
      break;
    case RUBY_GC_EVENT_END:
      rbtrace__send_event(1,
        "gc_end",
        'n'
      );
      break;
  }
}
#endif

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
    if (tracer->is_slow)
      rbtracer.num_slow--;

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
rbtracer_detach()
{
  rbtracer.attached_pid = 0;

  rbtracer.firehose = false;
  rbtracer.slow = false;
  rbtracer.slowcpu = false;
  rbtracer.gc = false;
  rbtracer.devmode = false;
  rbtracer.num_calls = 0;

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

  event_hook_remove();
#ifdef HAVE_RB_GC_ADD_EVENT_HOOK
  rb_gc_remove_event_hook(rbtrace_gc_event_hook);
#endif
}

static int
rbtracer_add(char *query, bool is_slow)
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

  size_t
    klass_begin = 0,
    klass_end = 0;

  bool is_singleton = false;

  VALUE
    klass = 0,
    self = 0;

  ID mid = 0;

  { // resolve query into its parts
    char
      *idx = NULL,
      *method = NULL;

    if (NULL != (idx = rindex(query, '.'))) {
      klass_begin = 0;
      klass_end = idx - query;
      is_singleton = true;

      *idx = 0;
      if (!rbtracer.devmode)
        self = rb_eval_string_protect(query, 0);
      *idx = '.';

      method = idx+1;

    } else if (NULL != (idx = rindex(query, '#'))) {
      klass_begin = 0;
      klass_end = idx - query;
      is_singleton = false;

      *idx = 0;
      if (!rbtracer.devmode)
        klass = rb_eval_string_protect(query, 0);
      *idx = '#';

      method = idx+1;

    } else {
      method = query;
    }

    if (method && *method) {
      mid = rb_intern(method);
    }
  }

  if (rbtracer.devmode) {
    if (!mid && (klass_begin == klass_end))
      goto out;
  } else {
    if (!mid && !klass && !self)
      goto out;
  }

  memset(tracer, 0, sizeof(*tracer));

  tracer->id = tracer_id;
  tracer->query = strdup(query);
  tracer->is_slow = is_slow;

  if (klass_end != klass_begin) {
    tracer->klass_name = tracer->query + klass_begin;
    tracer->klass_len = klass_end - klass_begin;
  }
  tracer->is_singleton = is_singleton;

  tracer->self = self;
  tracer->klass = klass;
  tracer->mid = mid;

  if (rbtracer.num == 0)
    event_hook_install();

  rbtracer.num++;
  if (tracer->is_slow)
    rbtracer.num_slow++;

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
rbtracer_watch(uint32_t threshold, bool cpu_time)
{
  if (!rbtracer.slow) {
    rbtracer.num_calls = 0;
    rbtracer.threshold = threshold;
    rbtracer.firehose = false;
    rbtracer.slow = true;
    rbtracer.slowcpu = cpu_time;

    event_hook_install();
  }
}

static void
msgq_teardown()
{
  pid_t pid = getpid();

  if (rbtracer.mqo_fd != -1) {
    close(rbtracer.mqo_fd);
    rbtracer.mqo_fd = -1;
  }

  if (rbtracer.mqi_id != -1 && rbtracer.mqi_key == (key_t)-pid) {
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
  int val;

  if (rbtracer.mqi_key != (key_t)-pid ||
      rbtracer.mqo_fd  == -1) {
    msgq_teardown();
  } else {
    return;
  }


  rbtracer.mqi_key = (key_t) -pid;
  rbtracer.mqi_id  = msgget(rbtracer.mqi_key, 0666 | IPC_CREAT);

  if (rbtracer.mqi_id == -1)
    fprintf(stderr, "msgget() failed to create msgq\n");


  rbtracer.mqo_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (rbtracer.mqo_fd == -1)
    fprintf(stderr, "socket() failed to create dgram\n");

  snprintf(rbtracer.mqo_addr.sun_path, sizeof(rbtracer.mqo_addr.sun_path), "/tmp/rbtrace-%d.sock", pid);
  rbtracer.mqo_len = SUN_LEN(&rbtracer.mqo_addr);

  val = 65536;
  setsockopt(rbtracer.mqo_fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(int));
#ifdef SO_NOSIGPIPE
  val = 1;
  setsockopt(rbtracer.mqo_fd, SOL_SOCKET, SO_NOSIGPIPE, &val, sizeof(int));
#endif
}

static void
rbtrace__process_event(msgpack_object cmd)
{
  if (cmd.type != MSGPACK_OBJECT_ARRAY)
    return;

  static int last_tracer_id = -1; // hax
  char query[BUF_SIZE];

  char code[BUF_SIZE+150];
  VALUE val = Qnil;

  msgpack_object_array ary;
  msgpack_object_str str;

  /* fprintf(stderr, "GOT: ");*/
  /* msgpack_object_print(stderr, cmd);*/
  /* fprintf(stderr, "\n");*/

  ary = cmd.via.array;

  if (ary.size < 1 ||
      ary.ptr[0].type != MSGPACK_OBJECT_STR)
    return;

  str = ary.ptr[0].via.str;

  if (0 == strncmp("attach", str.ptr, str.size)) {
    if (ary.size != 2 ||
        ary.ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
      return;

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
    }

    rbtracer_detach();

  } else if (0 == strncmp("watch", str.ptr, 5)) {
    if (ary.size != 2 ||
        ary.ptr[1].type != MSGPACK_OBJECT_POSITIVE_INTEGER)
      return;

    unsigned int msec = ary.ptr[1].via.u64;
    rbtracer_watch(msec, str.size > 5 /* watchcpu */);

  } else if (0 == strncmp("firehose", str.ptr, str.size)) {
    rbtracer.firehose = true;
    event_hook_install();

  } else if (0 == strncmp("add", str.ptr, str.size)) {
    if (ary.size != 3 ||
        ary.ptr[1].type != MSGPACK_OBJECT_STR ||
        ary.ptr[2].type != MSGPACK_OBJECT_BOOLEAN)
      return;

    str = ary.ptr[1].via.str;
    bool is_slow = ary.ptr[2].via.boolean;

    strncpy(query, str.ptr, str.size);
    query[str.size] = 0;
    last_tracer_id = rbtracer_add(query, is_slow);

  } else if (0 == strncmp("addexpr", str.ptr, str.size)) {
    if (ary.size != 2 ||
        ary.ptr[1].type != MSGPACK_OBJECT_STR)
      return;

    str = ary.ptr[1].via.str;

    strncpy(query, str.ptr, str.size);
    query[str.size] = 0;
    rbtracer_add_expr(last_tracer_id, query);

  } else if (0 == strncmp("gc", str.ptr, str.size)) {
    rbtracer.gc = true;
#ifdef HAVE_RB_GC_ADD_EVENT_HOOK
    rb_gc_add_event_hook(rbtrace_gc_event_hook, RUBY_GC_EVENT_START|RUBY_GC_EVENT_END);
#endif

  } else if (0 == strncmp("devmode", str.ptr, str.size)) {
    rbtracer.devmode = true;

  } else if (0 == strncmp("fork", str.ptr, str.size)) {
    pid_t outer = fork();

    if (outer == 0) {
      rb_eval_string_protect("$0 = \"[DEBUG] #{Process.ppid}\"", 0);

#ifdef PLATFORM_FREEBSD
      // The call setpgrp() is equivalent to setpgid(0,0).
      setpgid(0,0);
#else
      setpgrp();
#endif

      pid_t inner = fork();

      if (inner == 0) {
        // a ruby process will never have more than 20k
        // open file descriptors, right?
        int fd;
        for (fd=3; fd<20000; fd++)
          close(fd);

        // busy loop
        while (1) sleep(1);

        // don't return to ruby
        _exit(0);
      }

      rbtrace__send_event(1,
        "forked",
        inner == -1 ? 'b' : 'u',
        inner == -1 ? false : (uint32_t) inner
      );

      // kill off outer fork
      _exit(0);
    }

    if (outer != -1) {
      waitpid(outer, NULL, 0);
    }

  } else if (0 == strncmp("eval", str.ptr, str.size)) {
    if (ary.size != 2 ||
        ary.ptr[1].type != MSGPACK_OBJECT_STR)
      return;

    str = ary.ptr[1].via.str;

    strncpy(query, str.ptr, str.size);
    query[str.size] = 0;

    snprintf(code, BUF_SIZE+150, "(begin; %s; rescue Exception => e; e; end).inspect", query);
    val = rb_eval_string_protect(code, 0);

    if (TYPE(val) == T_STRING) {
      rbtrace__send_event(1,
        "evaled",
        's', RSTRING_PTR(val)
      );
    }

  }
}

static void
rbtrace__receive(void *data)
{
  msgq_setup();
  if (rbtracer.mqi_id == -1) return;

#ifdef HAVE_RB_DURING_GC
  if (rb_during_gc()) {
    rbtrace__send_event(0, "during_gc");
    return;
  }
#endif

  event_msg_t msg;
  int n = 0;

  while (true) {
    int ret = -1;

    for (n=0; n<10 && ret==-1; n++)
      ret = msgrcv(rbtracer.mqi_id, &msg, sizeof(msg)-sizeof(long), 0, IPC_NOWAIT);

    if (ret == -1) {
      break;
    } else {
      msgpack_unpacked unpacked;
      msgpack_unpacked_init(&unpacked);

      bool success = msgpack_unpack_next(&unpacked, msg.buf, sizeof(msg.buf), NULL);
      if (!success) continue;

      rbtrace__process_event(unpacked.data);
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

#if defined(HAVE_RB_POSTPONED_JOB_REGISTER_ONE) || !defined(RUBY_VM)
static void
sigurg(int signal)
{
#if defined(HAVE_RB_POSTPONED_JOB_REGISTER_ONE)
  rb_postponed_job_register_one(0, rbtrace__receive, 0);
#else
  rbtrace__receive(0);
#endif
}
#endif

#if !defined(HAVE_RB_POSTPONED_JOB_REGISTER_ONE) && defined(RUBY_VM)
static VALUE signal_handler_proc;
static VALUE
signal_handler_wrapper(VALUE arg, VALUE ctx)
{
  static int in_signal_handler = 0;
  if (in_signal_handler) return Qnil;

  in_signal_handler++;
  rbtrace__receive(0);
  in_signal_handler--;

  return Qnil;
}
#endif

void
Init_rbtrace()
{
  // hook into the gc
  gc_hook = Data_Wrap_Struct(rb_cObject, rbtrace_gc_mark, NULL, NULL);
  rb_global_variable(&gc_hook);

  // catch signal telling us to read from the msgq
#if defined(HAVE_RB_POSTPONED_JOB_REGISTER_ONE)
  signal(SIGURG, sigurg);
#elif defined(RUBY_VM)
  signal_handler_proc = rb_proc_new(signal_handler_wrapper, Qnil);
  rb_global_variable(&signal_handler_proc);
  rb_funcall(Qnil, rb_intern("trap"), 2, rb_str_new_cstr("URG"), signal_handler_proc);
#else
  signal(SIGURG, sigurg);
#endif

  // setup msgpack
  rbtracer.sbuf = msgpack_sbuffer_new();
  rbtracer.msgpacker = msgpack_packer_new(rbtracer.sbuf, msgpack_sbuffer_write);

  // zero out tracer
  memset(&rbtracer.list, 0, sizeof(rbtracer.list));

  // cleanup the msgq on exit
  atexit(msgq_teardown);
  rb_set_end_proc(ruby_teardown, 0);
}
