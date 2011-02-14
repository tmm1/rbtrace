# rbtrace: like strace, but for ruby code

rbtrace shows you method calls happening inside another ruby process in real
time.

rbtrace works on ruby 1.8 and 1.9, running on linux or mac osx.

## usage

### require rbtrace into a process

    % cat server.rb
    require 'ext/rbtrace'

    class String
      def multiply_vowels(num)
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
    [1] 95532

### trace a function using the process's pid

    % ./bin/rbtrace $! sleep

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

    % ./bin/rbtrace $! sleep Dir.chdir Dir.pwd Process.pid "String#gsub" "String#*"

    Dir.chdir
       Dir.pwd <0.000094>
       Process.pid <0.000016>
       String#gsub
          String#* <0.000020>
          String#* <0.000020>
       String#gsub <0.000072>
       Kernel#sleep <0.369630>
    Dir.chdir <0.370220>

    Dir.chdir
       Dir.pwd <0.000088>
       Process.pid <0.000017>
       String#gsub
          String#* <0.000020>
          String#* <0.000020>
       String#gsub <0.000071>
    ^C./bin/rbtrace:113: Interrupt

### trace all functions in a class/module

    % ./bin/rbtrace $! "Kernel#"

    Kernel#proc <0.000071>
    Kernel#rand <0.000029>
    Kernel#sleep <0.331857>
    Kernel#proc <0.000019>
    Kernel#rand <0.000010>
    Kernel#sleep <0.296361>
    Kernel#proc <0.000021>
    Kernel#rand <0.000012>
    Kernel#sleep <0.281067>
    ^C./bin/rbtrace:113: Interrupt

### get values of variables and other expressions

    % ./bin/rbtrace $! "String#gsub(self)" "String#*(self)" "String#multiply_vowels(self, self.length, num)"

    String#multiply_vowels(self="hello", self.length=5, num=3)
       String#gsub(self="hello")
          String#*(self="e") <0.000021>
          String#*(self="o") <0.000019>
       String#gsub <0.000097>
    String#multiply_vowels <0.000203>

    ^C./bin/rbtrace:113: Interrupt

### watch for method calls slower than 250ms

    % ./bin/rbtrace $! watch 250
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

## todo

* add some simple tests
* switch ipc to msgpack instead of csv
* use proper option parser to bin/rbtrace
  * add -t and -tt for start timestamps
  * add -o to write trace output to a file
  * add -p to provide pid
  * add -c to read trace definitions from conf file
* investigate mach_msg on osx since msgget(2) has hard kernel limits

