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

ruby server.rb &
export PID=$!

trap cleanup INT TERM
cleanup() {
  kill $PID
  wait $PID || true
}

trace() {
  echo ------------------------------------------
  echo ./bin/rbtrace -p $PID $*
  echo ------------------------------------------
  ./bin/rbtrace -p $PID $* &
  sleep 2
  kill $!
  wait $! || true
  echo
}

trace -m Test.run --devmode
trace -m sleep
trace -m sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"
trace -m "Kernel#"
trace -m "String#gsub(self,@test)" "String#*(self,__source__)" "String#multiply_vowels(self,self.length,num)"
trace --gc --slow=200
trace --gc -m Dir.
trace --slow=250
trace --slow=250 --slow-methods sleep
trace --gc -m Dir. --slow=250 --slow-methods sleep
trace --gc -m Dir. --slow=250
trace -m Process. Dir.pwd "Proc#call"
trace --firehose

cleanup
