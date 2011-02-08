#include <inttypes.h>
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
  VALUE self;
  VALUE klass;
  ID mid;

  int calls;
  uint64_t call_times[MAX_CALLS];

  int c_calls;
  uint64_t c_call_times[MAX_CALLS];
};
typedef struct rbtracer_t rbtracer_t;

static rbtracer_t tracers[MAX_TRACERS];
static unsigned int num_tracers = 0;

static int in_event_hook = 0;
static int nesting = 0;

static void
event_hook(rb_event_t event, NODE *node, VALUE self, ID mid, VALUE klass)
{
  if (num_tracers == 0) return;
  if (in_event_hook) return;
  if (mid == ID_ALLOCATOR) return;
  in_event_hook++;

  if (klass) {
    if (TYPE(klass) == T_ICLASS) {
      klass = RBASIC(klass)->klass;
    }
  }

  int i;
  uint64_t usec, diff;
  rbtracer_t *tracer = NULL;

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
    case RUBY_EVENT_CALL:
      if (tracer->calls < MAX_CALLS) {
        tracer->call_times[ tracer->calls ] = usec;

        if (nesting) {
          printf("\n");
          for (i=0; i<nesting; i++)
            printf("   ");
        }

        if (klass) {
          if (FL_TEST(klass, FL_SINGLETON)) {
            klass = self;
          }

          printf("%s%c", rb_class2name(klass), klass==self ? '.' : '#');
        }

        printf("%s ", rb_id2name(mid));
      }
      tracer->calls++;

      nesting++;
      break;

    case RUBY_EVENT_RETURN:
      if (nesting > 0)
        nesting--;

      if (tracer->calls > 0) {
        tracer->calls--;

        if (tracer->calls < MAX_CALLS) {
          diff = usec - tracer->call_times[ tracer->calls ];

          if (nesting) {
            for (i=0; i<nesting; i++)
              printf("   ");
          }
          printf("<%" PRIu64 ">\n", diff);
        }
      }
      break;

    case RUBY_EVENT_C_CALL:
      break;

    case RUBY_EVENT_C_RETURN:
      break;
  }

out:
  in_event_hook--;
}

static int
rbtracer_add(VALUE self, VALUE klass, ID mid)
{
  if (num_tracers >= MAX_TRACERS) return -1;
  if (num_tracers == 0) {
    rb_add_event_hook(
      event_hook,
      RUBY_EVENT_CALL   | RUBY_EVENT_C_CALL |
      RUBY_EVENT_RETURN | RUBY_EVENT_C_RETURN
    );
  }

  memset(&tracers[num_tracers], 0, sizeof(rbtracer_t));
  tracers[num_tracers].self = self;
  tracers[num_tracers].klass = klass;
  tracers[num_tracers].mid = mid;

  return num_tracers++;
}

static VALUE
rbtrace(VALUE self, VALUE method)
{
  Check_Type(method, T_STRING);

  VALUE klass = 0, obj = 0;
  ID mid;
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

  if (rbtracer_add(obj, klass, mid) > -1)
    return Qtrue;
  else
    return Qfalse;
}

void
Init_rbtrace()
{
  rb_define_method(rb_cObject, "rbtrace", rbtrace, 1);
}
