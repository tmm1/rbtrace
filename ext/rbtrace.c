#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include <ruby.h>
#include <node.h>

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

  VALUE self;
  VALUE klass;
  ID mid;
};
typedef struct rbtracer_t rbtracer_t;

static rbtracer_t tracers[MAX_TRACERS];
static unsigned int num_tracers = 0;

static int in_event_hook = 0;
static int nesting = 0;
FILE *output = NULL;

static void
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  if (num_tracers == 0) return;
  if (in_event_hook) return;
  if (mid == ID_ALLOCATOR) return;
  in_event_hook++;

  int i;
  uint64_t usec, diff;
  rbtracer_t *tracer = NULL;
  bool singleton = 0;

  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC(klass)->klass;
    }
    singleton = FL_TEST(klass, FL_SINGLETON);
  }

  for (i=0; i<num_tracers; i++) {
    if (tracers[i].mid == mid) {
      if (!tracers[i].klass || tracers[i].klass == klass) {
        if (!tracers[i].self || tracers[i].self == self) {
          tracer = &tracers[i];
        }
      }
    }
  }
  if (!tracer) goto out;

  usec = timeofday_usec();

  switch (event) {
    case RUBY_EVENT_C_CALL:
    case RUBY_EVENT_CALL:
      fprintf(
        output,
        "%llu,%s,%d,%s,%d,%s\n",
        usec,
        event == RUBY_EVENT_CALL ? "call" : "ccall",
        tracer->id,
        rb_id2name(mid),
        singleton,
        klass ? rb_class2name(singleton ? self : klass) : ""
      );
      break;

    case RUBY_EVENT_C_RETURN:
    case RUBY_EVENT_RETURN:
      fprintf(
        output,
        "%llu,%s,%d\n",
        usec,
        event == RUBY_EVENT_RETURN ? "return" : "creturn",
        tracer->id
      );
      break;
  }

out:
  in_event_hook--;
}

static int
rbtracer_add(char *query, VALUE self, VALUE klass, ID mid)
{
  int tracer_id = -1;
  uint64_t usec = timeofday_usec();

  if (num_tracers >= MAX_TRACERS) goto out;
  if (num_tracers == 0) {
    rb_add_event_hook(
      event_hook,
      RUBY_EVENT_CALL   | RUBY_EVENT_C_CALL |
      RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN
    );
  }

  memset(&tracers[num_tracers], 0, sizeof(rbtracer_t));

  tracers[num_tracers].id = num_tracers;
  tracers[num_tracers].self = self;
  tracers[num_tracers].klass = klass;
  tracers[num_tracers].mid = mid;

  tracer_id = num_tracers++;

out:
  fprintf(
    output,
    "%llu,new,%d,%s\n",
    usec,
    tracer_id,
    query
  );
  return tracer_id;
}

static VALUE
rbtrace(VALUE self, VALUE method)
{
  Check_Type(method, T_STRING);

  VALUE klass = 0, obj = 0;
  ID mid;
  int tracer_id = -1;
  char *str = RSTRING(method)->ptr;
  char *index;

  if (NULL != (index = rindex(str, '.'))) {
    *index = 0;
    obj = rb_eval_string_protect(str, 0);
    *index = '.';

    mid = rb_intern(index+1);
  } else if (NULL != (index = rindex(str, '#'))) {
    *index = 0;
    klass = rb_eval_string_protect(str, 0);
    *index = '#';

    mid = rb_intern(index+1);
  } else {
    mid = rb_intern(str);
  }

  tracer_id = rbtracer_add(str, obj, klass, mid);
  return tracer_id == -1 ? Qfalse : Qtrue;
}

void
Init_rbtrace()
{
  if (!output)
    output = stdout;

  rb_define_method(rb_cObject, "rbtrace", rbtrace, 1);
}
