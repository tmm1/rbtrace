#!/bin/sh
set -e

cd ext
ruby extconf.rb
make clean
make
cd ..

bundle check
export RUBYOPT="-I."

ruby server.rb &
export PID=$!

trap cleanup SIGINT SIGTERM
cleanup() {
  kill $PID
  wait $PID || true
}

trace() {
  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID $*
  echo ------------------------------------------
  ./bin/rbtrace -p $PID -r 3 $* &
  sleep 2
  kill $!
  wait $! || true
  echo
}

trace -m sleep
trace -m sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"
trace -m "Kernel#"
trace -m "String#gsub(self,@test)" "String#*(self,__source__)" "String#multiply_vowels(self,self.length,num)"
trace --slow=250
trace --firehose

cleanup
