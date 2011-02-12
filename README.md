# rbtrace

like strace, but for ruby code

## usage

### require rbtrace into a process

    % cat server.rb
    require 'ext/rbtrace'

    while true
      proc {
        Dir.chdir("/tmp") do
          Dir.pwd
          Process.pid
          ("hi"*5).gsub('hi', 'hello')
          sleep rand*0.5
        end
      }.call
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

    % ./bin/rbtrace 95532 Dir.chdir sleep Dir.pwd Process.pid "String#gsub" "String#*"

    Dir.chdir
       Dir.pwd <0.000029>
       Process.pid <0.000007>
       String#* <0.000008>
       String#gsub <0.000032>
       Kernel#sleep <0.364838>
    <0.365453>
    Dir.chdir
       Dir.pwd <0.000028>
       Process.pid <0.000007>
       String#* <0.000008>
       String#gsub <0.000019>
       Kernel#sleep <0.068568>
    <0.068808>
    Dir.chdir
       Dir.pwd <0.000042>
       Process.pid <0.000067>
       String#* <0.000012>
       String#gsub <0.000015>
       Kernel#sleep <0.369132>
    <0.370278>
    ^C./bin/rbtrace:113: Interrupt

### watch for method calls slower than 250ms

    % ./bin/rbtrace 95532 watch
          Kernel#sleep <0.402916>
       Dir.chdir <0.403122>
    Proc#call <0.403152>

          Kernel#sleep <0.390635>
       Dir.chdir <0.390937>
    Proc#call <0.390983>

          Kernel#sleep <0.399413>
       Dir.chdir <0.399753>
    Proc#call <0.399797>

    ^C./bin/rbtrace:113: Interrupt

