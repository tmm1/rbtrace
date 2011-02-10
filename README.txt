    ~/code/rbtrace (master*) % cat server.rb
    require 'rbtrace'

    while true
      Dir.chdir("/tmp") do
        sleep rand*0.5
      end
    end

    ~/code/rbtrace (master*) % ruby server.rb &
    [1] 77380

    ~/code/rbtrace (master*) % ./bin/rbtrace 77380 sleep
    Kernel#sleep <0.240676>
    Kernel#sleep <0.322608>
    Kernel#sleep <0.409339>
    Kernel#sleep <0.257449>
    Kernel#sleep <0.077697>
    ^C./bin/rbtrace:95: Interrupt

    ~/code/rbtrace (master*) % ./bin/rbtrace 77380 Dir.chdir
    Dir.chdir <0.351901>
    Dir.chdir <0.148917>
    Dir.chdir <0.092665>
    Dir.chdir <0.040374>
    Dir.chdir <0.022981>
    Dir.chdir <0.023986>
    ^C./bin/rbtrace:95: Interrupt
