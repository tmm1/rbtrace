#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include <ruby.h>
#include <node.h>

static uint64_t
timeofday_us()
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  /* return (uint64_t)tv.tv_sec*1e3 + (uint64_t)tv.tv_usec*1e-3;*/
  return (uint64_t)tv.tv_sec*1e6 + (uint64_t)tv.tv_usec;
}

ID current;

#define DEFINE_EVENT_HOOK(name) \
  static rb_event_hook_func_t \
  name##_event_hook(rb_event_t event, NODE *node, VALUE self, ID id, VALUE klass)

/* static uint64_t*/
/* call_counter = 0;*/

/* DEFINE_EVENT_HOOK(call)*/
/* {*/
/*   call_counter++;*/

/*   uint64_t usec = timeofday_us();*/
/*   printf("%" PRIu64 ": called(%d) %s\n", usec, call_counter, rb_id2name(id));*/
/* }*/

/* DEFINE_EVENT_HOOK(return)*/
/* {*/
/*   uint64_t usec = timeofday_us();*/
/*   printf("%" PRIu64 ": returned(%d)\n", usec, call_counter);*/

/*   call_counter--;*/
/* }*/

static uint64_t
c_call_counter = 0,
watched_counter = 0,
watched_time = 0;

static int
waiting_inside = 0;

DEFINE_EVENT_HOOK(line)
{
  if (waiting_inside == 1) {
    printf("ohai\n");
    waiting_inside = 0;
  }
  rb_remove_event_hook(line_event_hook);
}

DEFINE_EVENT_HOOK(c_call)
{
  c_call_counter++;

  if (current != id) return;
  if (watched_counter != 0) return;

  uint64_t usec = timeofday_us();

  watched_counter = c_call_counter;
  watched_time = usec;
  waiting_inside = 1;

  printf("-------\n");
  rb_eval_string_protect("p local_variables", 0);
  rb_eval_string_protect("p [key, sup]", 0);
  printf("%" PRIu64 ": called(%d) %s#%s\n", usec, c_call_counter, rb_class2name(klass), rb_id2name(id));

  /* rb_add_event_hook(line_event_hook, RUBY_EVENT_LINE);*/
}

DEFINE_EVENT_HOOK(c_return)
{
  if (watched_counter != 0 && watched_counter == c_call_counter) {
    uint64_t usec = timeofday_us();
    printf("%" PRIu64 ": returned(%d) %s in %d\n", usec, c_call_counter, rb_id2name(id), usec-watched_time);
    rb_eval_string_protect("p [:out, local_variables]", 0);

    waiting_inside = 0;
    watched_counter = 0;
    /* rb_remove_event_hook(line_event_hook);*/
  }

  c_call_counter--;
}

void
Init_rbtrace()
{
  current = rb_intern("[]=");

  /* rb_add_event_hook(call_event_hook, RUBY_EVENT_CALL);*/
  /* rb_add_event_hook(return_event_hook, RUBY_EVENT_RETURN);*/

  rb_add_event_hook(c_call_event_hook, RUBY_EVENT_C_CALL|RUBY_EVENT_CALL);
  rb_add_event_hook(c_return_event_hook, RUBY_EVENT_C_RETURN|RUBY_EVENT_RETURN);
}
