# rbtrace

like strace, but for ruby code

## usage

### require rbtrace into a process

    % cat server.rb
    require 'ext/rbtrace'

    while true
      Dir.chdir("/tmp") do
        Dir.pwd
        Process.pid
        sleep rand*0.5
      end
    end

### run the process

    % ruby server.rb &
    [1] 95532

### trace a function using the process's pid

    % ./bin/rbtrace 95532 sleep

    Kernel#sleep <0.329042>
    Kernel#sleep <0.035732>
    Kernel#sleep <0.291189>
    Kernel#sleep <0.355800>
    Kernel#sleep <0.238176>
    Kernel#sleep <0.147345>
    Kernel#sleep <0.238686>
    Kernel#sleep <0.239668>
    Kernel#sleep <0.035512>
    ^C./bin/rbtrace:113: Interrupt

### trace multiple functions

    % ./bin/rbtrace 95532 Dir.chdir sleep Dir.pwd Process.pid

    Dir.chdir
       Dir.pwd    <0.000057>
       Process.pid    <0.000015>
       Kernel#sleep    <0.153867>
    <0.154228>
    Dir.chdir
       Dir.pwd    <0.000221>
       Process.pid    <0.000008>
       Kernel#sleep    <0.427436>
    <0.427923>
    Dir.chdir
       Dir.pwd    <0.000057>
       Process.pid    <0.000013>
       Kernel#sleep    <0.267780>
    <0.268102>
    ^C./bin/rbtrace:113: Interrupt

