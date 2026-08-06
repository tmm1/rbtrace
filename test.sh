#!/bin/sh
set -e

bundle check || bundle install

cd ext
[ -f Makefile ] && make clean
ruby extconf.rb
make
cd ..

bundle check
export RUBYOPT="-I.:lib"

bundle exec ruby server.rb &
export PID=$!

trap cleanup EXIT INT TERM
cleanup() {
  if [ -n "${PID:-}" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    PID=
  fi
}

trace() {
  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID "$@"
  echo ------------------------------------------
  bundle exec ./bin/rbtrace -p $PID "$@" &
  sleep 2
  kill $! || true
  wait $! || true
  echo
}

assert_trace() {
  expected="$1"
  shift

  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID "$@"
  echo ------------------------------------------
  if ! output=$(bundle exec ./bin/rbtrace -p $PID "$@" 2>&1); then
    echo "$output"
    return 1
  fi
  echo "$output"
  echo "$output" | grep -F "$expected" >/dev/null
  echo
}

assert_dump() {
  dump_type="$1"
  dump=$(mktemp)
  rm -f "$dump"

  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID --"$dump_type"="$dump"
  echo ------------------------------------------
  if ! output=$(bundle exec ./bin/rbtrace -p $PID --"$dump_type"="$dump" 2>&1); then
    echo "$output"
    rm -f "$dump" "$dump.tmp"
    return 1
  fi
  echo "$output"

  tries=0
  while [ "$tries" -lt 50 ] && [ ! -s "$dump" ]; do
    sleep 0.1
    tries=$((tries + 1))
  done

  if [ ! -s "$dump" ]; then
    echo "Expected $dump_type to be written to $dump"
    rm -f "$dump" "$dump.tmp"
    return 1
  fi

  rm -f "$dump" "$dump.tmp"
  echo
}

assert_timed_trace() {
  expected="$1"
  shift
  output=$(mktemp)

  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID "$@"
  echo ------------------------------------------
  bundle exec ./bin/rbtrace -p $PID "$@" >"$output" 2>&1 &
  tracer_pid=$!
  sleep 2
  kill "$tracer_pid" 2>/dev/null || true
  wait "$tracer_pid" 2>/dev/null || true
  cat "$output"
  if ! grep -F "$expected" "$output" >/dev/null; then
    rm -f "$output"
    return 1
  fi
  rm -f "$output"
  echo
}

trace -m Test.run --devmode
trace -m sleep
trace -m sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"
trace -m "Kernel#"
trace -m "String#gsub(self,@test)" "String#*(self,__source__)" "String#multiply_vowels(self,self.length,num)"
assert_timed_trace 'String#upcase(self.class=String)' -m 'String#upcase(self.class)'
assert_trace '=> 2' -e 'p(1 + 1)'
assert_dump heapdump
assert_dump shapesdump
trace --gc --slow=200
trace --gc -m Dir.
trace --slow=250
trace --slow=250 --slow-methods sleep
trace --gc -m Dir. --slow=250 --slow-methods sleep
trace --gc -m Dir. --slow=250
trace -m Process. Dir.pwd "Proc#call"
trace --firehose

echo ------------------------------------------
echo interactive irb output
echo ------------------------------------------
bundle exec ruby test/interactive_irb_test.rb $PID

cleanup
