# rbtrace: like strace, but for ruby code

rbtrace shows you method calls happening inside another ruby process in real
time.

rbtrace works on ruby 1.8 through 2.2 (and beyond), running on linux or mac
osx.

rbtrace is designed to have minimal overhead, and should be safe to run
in production.

## usage

    % gem install rbtrace
    % rbtrace --help

## tracer types

rbtrace has several different tracing modes.

### firehose: show everything

    % rbtrace -p <PID> --firehose

### slow: show any method calls that take longer than `<N>` milliseconds

    % rbtrace -p <PID> --slow=<N>

### methods: trace calls to specific methods

    % rbtrace -p <PID> --methods "Kernel#sleep" "Proc#call"

### gc: trace garbage collections

    % rbtrace -p <PID> --gc

### notes

`--firehose` is not reliable on osx.

`--slow`, `--gc` and `--methods` can be combined.

## predefined tracers

rbtrace also includes a set of [predefined tracers](https://github.com/tmm1/rbtrace/tree/master/tracers)
for popular ruby libraries and functions.

### trace calls to activerecord adapters and any i/o functions

    % rbtrace -p <PID> -c activerecord io

## detailed example

### require rbtrace into a process

    % cat server.rb
    require 'rbtrace'

    class String
      def multiply_vowels(num)
        @test = 123
        gsub(/[aeiou]/){ |m| m*num }
      end
    end

    while true
      proc {
        Dir.chdir("/tmp") do
          Dir.pwd
          Process.pid
          'hello'.multiply_vowels(3)
          sleep rand*0.5
        end
      }.call
    end

### run the process

    % ruby server.rb &
    [1] 87854

### trace a function using the process's pid

    % rbtrace -p 87854 -m sleep
    *** attached to process 87854

    Kernel#sleep <0.138615>
    Kernel#sleep <0.147726>
    Kernel#sleep <0.318728>
    Kernel#sleep <0.338173>
    Kernel#sleep <0.373004>
    Kernel#sleep
    *** detached from process 87854

### trace everything

    % rbtrace -p 87854 --firehose
    *** attached to process 87938

    Kernel#proc <0.000082>
    Proc#call
       Dir.chdir
          Dir.pwd <0.000060>
          Process.pid <0.000021>
          String#multiply_vowels
             String#gsub
                String#* <0.000023>
                String#* <0.000022>
             String#gsub <0.000127>
          String#multiply_vowels <0.000175>
          Kernel#rand <0.000018>
          Float#* <0.000022>
          Kernel#sleep <0.344281>
       Dir.chdir <0.344858>
    Proc#call <0.344908>
    *** detached from process 87938

### trace specific functions

    % rbtrace -p 87854 -m sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"
    *** attached to process 87854

    Dir.chdir
       Dir.pwd <0.000023>
       Process.pid <0.000008>
       String#gsub
          String#* <0.000008>
          String#* <0.000007>
       String#gsub <0.000050>
       Kernel#sleep <0.498809>
    Dir.chdir <0.499025>

    Dir.chdir
       Dir.pwd <0.000024>
       Process.pid <0.000008>
       String#gsub
          String#* <0.000008>
          String#* <0.000007>
       String#gsub <0.000050>
       Kernel#sleep
    *** detached from process 87854

### trace all functions in a class/module

    % rbtrace -p 87854 -m "Kernel#"
    *** attached to process 87854

    Kernel#proc <0.000062>
    Kernel#rand <0.000010>
    Kernel#sleep <0.218677>
    Kernel#proc <0.000016>
    Kernel#rand <0.000010>
    Kernel#sleep <0.195914>
    Kernel#proc <0.000016>
    Kernel#rand <0.000009>
    Kernel#sleep
    *** detached from process 87854

### get values of variables and other expressions

    % rbtrace -p 87854 -m "String#gsub(self, @test)" "String#*(self, __source__)" "String#multiply_vowels(self, self.length, num)"
    *** attached to process 87854

    String#multiply_vowels(self="hello", self.length=5, num=3)
       String#gsub(self="hello", @test=123)
          String#*(self="e", __source__="server.rb:6") <0.000020>
          String#*(self="o", __source__="server.rb:6") <0.000018>
       String#gsub <0.000085>
    String#multiply_vowels <0.000198>

    String#multiply_vowels(self="hello", self.length=5, num=3)
       String#gsub(self="hello", @test=123)
          String#*(self="e", __source__="server.rb:6") <0.000020>
          String#*(self="o", __source__="server.rb:6") <0.000020>
       String#gsub <0.000102>
    String#multiply_vowels <0.000218>

    *** detached from process 87854

### watch for method calls slower than 250ms

    % rbtrace -p 87854 --slow=250
    *** attached to process 87854
          Kernel#sleep <0.459628>
       Dir.chdir <0.459828>
    Proc#call <0.459849>

          Kernel#sleep <0.484666>
       Dir.chdir <0.484804>
    Proc#call <0.484818>

    *** detached from process 87854

## todo

* add triggers to start tracing slow methods only inside another method
* add watch expressions to fire tracers only when an expression is true
* add special expressions for method args (_arg0_, _arguments_)
* optimize local variable lookup to avoid instance_eval
* investigate mach_msg on osx since msgget(2) has hard kernel limits
