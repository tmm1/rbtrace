#!/bin/sh
set -e

make -C ext 2>&1 >/dev/null
ruby server.rb &
export PID=$!

trap cleanup SIGINT SIGTERM
cleanup() {
  kill $PID
  wait $PID || true
}

trace() {
  echo ------------------------------------------------------------------------------------------------------
  echo ./bin/rbtrace $PID $*
  echo ------------------------------------------------------------------------------------------------------
  ./bin/rbtrace $PID $* &
  sleep 2
  kill $!
  wait $! || true
  echo
}

trace sleep
trace sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"
trace "Kernel#"
trace "String#gsub(self,@test)" "String#*(self)" "String#multiply_vowels(self,self.length,num,__source__)"
trace watch 250
trace firehose

cleanup
