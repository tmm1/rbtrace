require 'optimist'
require 'rbtrace/rbtracer'
require 'rbtrace/version'
require 'rbtrace/converter'

class RBTraceCLI
  singleton_class.send(:attr_accessor, :tracer)

  # Suggest increasing the maximum number of bytes allowed on
  # a message queue to 1MB.
  #
  # This defaults to 16k on Linux, and is hardcoded to 2k in OSX kernel.
  #
  # Returns nothing.
  def self.check_msgmnb
    if File.exist?(msgmnb = "/proc/sys/kernel/msgmnb")
      curr = File.read(msgmnb).to_i
      max = 1024*1024
      cmd = "sysctl kernel.msgmnb=#{max}"

      if curr < max
        if Process.uid == 0
          STDERR.puts "*** running `#{cmd}` for you to prevent losing events (currently: #{curr} bytes)"
          system(cmd)
        else
          STDERR.puts "*** run `sudo #{cmd}` to prevent losing events (currently: #{curr} bytes)"
        end
      end
    end
  end

  # Look for any message queues pairs (pid/-pid) that no longer have an
  # associated process alive, and remove them.
  #
  # Returns nothing.
  def self.cleanup_queues
    if (pids = `ps ax -o pid`.split("\n").map{ |p| p.strip.to_i }).any?
      ipcs = `ipcs -q`.split("\n").grep(/^(q|0x)/).map{ |line| line[/(0x[a-f0-9]+)/,1] }.compact
      ipcs.each do |ipci|
        next if ipci.match(/^0xf/)

        qi = ipci.to_i(16)
        qo = 0xffffffff - qi + 1
        ipco = "0x#{qo.to_s(16)}"

        if ipcs.include?(ipco) and !pids.include?(qi)
          STDERR.puts "*** removing stale message queue pair: #{ipci}/#{ipco}"
          system("ipcrm -Q #{ipci} -Q #{ipco}")
        end
      end
    end
  end

  def self.run
    check_msgmnb
    cleanup_queues

    parser = Optimist::Parser.new do
      version <<-EOS
rbtrace: like strace, but for ruby code
  version #{RBTracer::VERSION}
  (c) 2013 Aman Gupta (tmm1)
  http://github.com/tmm1/rbtrace
EOS

      banner <<-EOS
rbtrace shows you method calls happening inside another ruby process in real time.

to use rbtrace, simply `require "rbtrace"` in your ruby app.

for examples and more information, see http://github.com/tmm1/rbtrace

Usage:

  rbtrace --exec <CMD>     # run and trace <CMD>
  rbtrace --pid <PID+>     # trace the given process(es)
  rbtrace --ps <CMD>       # look for running <CMD> processes to trace

  rbtrace -o <FILE>        # write output to file
  rbtrace -t               # show method call start time
  rbtrace -n               # hide duration of each method call
  rbtrace -r 3             # use 3 spaces to nest method calls

Tracers:

  rbtrace --firehose       # trace all method calls
  rbtrace --slow=250       # trace method calls slower than 250ms
  rbtrace --methods a b c  # trace calls to given methods
  rbtrace --gc             # trace garbage collections

  rbtrace -c io            # trace common input/output functions
  rbtrace -c eventmachine  # trace common eventmachine functions
  rbtrace -c my.tracer     # trace all methods listed in my.tracer

Method Selectors:

  sleep                    # any instance or class method named sleep
  String#gsub              # specific instance method
  Process.pid              # specific class method
  Dir.                     # any class methods in Dir
  Fixnum#                  # any instance methods of Fixnum

Trace Expressions:

  method(self)             # value of self at method invocation
  method(@ivar)            # value of given instance variable
  method(arg1, arg2)       # value of argument local variables
  method(self.attr)        # value of arbitrary ruby expression
  method(__source__)       # source file/line of callsite


All Options:\n

EOS
      opt :exec,
        "spawn new ruby process and attach to it",
        :type => :strings,
        :short => nil

      opt :pid,
        "pid of the ruby process to trace",
        :type => :ints,
        :short => '-p'

      opt :ps,
        "find any matching processes to trace",
        :type => :string,
        :short => nil

      opt :firehose,
        "show all method calls",
        :short => '-f'

      opt :slow,
        "watch for method calls slower than 250 milliseconds",
        :default => 250,
        :short => '-s'

      opt :slowcpu,
        "watch for method calls slower than 250 milliseconds (cpu time only)",
        :default => 250,
        :short => nil

      opt :slow_methods,
        "method(s) to restrict --slow to",
        :type => :strings

      opt :methods,
        "method(s) to trace (valid formats: sleep String#gsub Process.pid Kernel# Dir.)",
        :type => :strings,
        :short => '-m'

      opt :gc,
        "trace garbage collections"

      opt :start_time,
        "show start time for each method call",
        :short => '-t'

      opt :no_duration,
        "hide time spent in each method call",
        :default => false,
        :short => '-n'

      opt :output,
        "write trace to filename",
        :type => String,
        :short => '-o'

      opt :append,
        "append to output file instead of overwriting",
        :short => '-a'

      opt :prefix,
        "prefix nested method calls with N spaces",
        :default => 2,
        :short => '-r'

      opt :config,
        "config file",
        :type => :strings,
        :short => '-c'

      opt :devmode,
        "assume the ruby process is reloading classes and methods"

      opt :fork,
        "fork a copy of the process for debugging (so you can attach gdb.rb)"

      opt :eval,
        "evaluate a ruby expression in the process",
        :type => String,
        :short => '-e'

      opt :interactive,
        "interactive",
        :type => String,
        :default => 'irb',
        :short => '-i'

      opt :backtraces,
        "get backtraces for all threads in current process, -1 denotes all frames",
        :type => String,
        :default => '-1',
        :short => '-b'

      opt :backtrace,
        "get lines from the current backtrace in the process",
        :type => :int

      opt :wait,
        "seconds to wait before attaching to process",
        :default => 0,
        :short => nil

      opt :timeout,
        "seconds to wait before giving up on attach/detach/eval",
        :default => 5

      opt :memory,
        "report on process memory usage"


      opt :heapdump,
        "generate a heap dump for the process in FILENAME",
        :default => "AUTO",
        :short => "-h"

      opt :shapesdump,
        "generate a shapes dump for the process in FILENAME",
        :default => "AUTO"

      opt :convert,
        "convert a trace file to another format",
        :default => "flamegraph"
    end

    opts = Optimist.with_standard_exception_handling(parser) do
      raise Optimist::HelpNeeded if ARGV.empty?
      parser.stop_on '--exec'
      parser.parse(ARGV)
    end

    if ARGV.first == '--exec'
      ARGV.shift
      opts[:exec_given] = true
      opts[:exec] = ARGV.dup
      ARGV.clear
    end

    unless %w[ fork eval interactive backtrace backtraces slow slowcpu firehose methods config gc memory heapdump convert].find{ |n| opts[:"#{n}_given"] }
      $stderr.puts "Error: --slow, --slowcpu, --gc, --firehose, --methods, --interactive, --backtraces, --backtrace, --memory, --heapdump, --shapesdump, --convert or --config required."
      $stderr.puts "Try --help for help."
      exit(-1)
    end

    if opts[:fork_given] and opts[:pid].size != 1
      parser.die :fork, '(can only be invoked with one pid)'
    end

    if opts[:exec_given]
      if opts[:pid_given]
        parser.die :exec, '(cannot exec and attach to pid)'
      end
      if opts[:fork_given]
        parser.die :fork, '(cannot fork inside newly execed process)'
      end
    end

    methods, smethods = [], []

    if opts[:methods_given]
      methods += opts[:methods]
    end
    if opts[:slow_methods_given]
      smethods += opts[:slow_methods]
    end

    if opts[:config_given]
      Array(opts[:config]).each do |config|
        file = [
          config,
          File.expand_path("../../../tracers/#{config}.tracer", __FILE__)
        ].find{ |f| File.exist?(f) }

        unless file
          parser.die :config, '(file does not exist)'
        end

        File.readlines(file).each do |line|
          line.strip!
          next if line =~ /^#/
          next if line.empty?

          methods << line
        end
      end
    end

    tracee = nil

    if opts[:ps_given]
      list = `ps aux`.split("\n")
      filtered = list.grep(Regexp.new opts[:ps])
      filtered.reject! do |line|
        line =~ /^\w+\s+(#{Process.pid}|#{Process.ppid})\s+/ # cannot trace self
      end

      if filtered.size > 0
        max_len = filtered.size.to_s.size

        STDERR.puts "*** found #{filtered.size} process#{filtered.size == 1 ? "" : "es"} matching #{opts[:ps].inspect}"
        filtered.each_with_index do |line, i|
          prefix = "   [#{(i+1).to_s.rjust(max_len)}]   "
          if filtered.length == 1
            prefix = ""
          end
          STDERR.puts "#{prefix}#{line.strip}"
        end

        if filtered.length > 1
          STDERR.puts   "   [#{'0'.rjust(max_len)}]   all #{filtered.size} processes"
        end

        while true
          STDERR.sync = true

          if filtered.length > 1
            STDERR.print "*** trace which processes? (0/1,4): "
          end

          begin
            if filtered.length == 1
              input = "1"
            else
              input = gets
            end
          rescue Interrupt
            exit 1
          end

          if input =~ /^(\d+,?)+$/
            if input.strip == '0'
              pids = filtered.map do |line|
                line.split[1].to_i
              end
            else
              indices = input.split(',').map(&:to_i)
              pids = indices.map do |i|
                if i > 0 and line = filtered[i-1]
                  line.split[1].to_i
                end
              end
            end

            unless pids.include?(nil)
              opts[:pid] = pids
              break
            end
          end
        end
      else
        STDERR.puts "*** could not find any processes matching #{opts[:ps].inspect}"
        exit 1
      end
    end

    if opts[:convert_given]
      require 'rbtrace/converter'
      converter = RBTrace::Converter.build(opts[:convert], ARGF)
      converter.convert

      return
    end

    if opts[:exec_given]
      tracee = fork{
        Process.setsid
        ENV['RUBYOPT'] = "-r#{File.expand_path('../../rbtrace',__FILE__)}"
        exec(*opts[:exec])
      }
      STDERR.puts "*** spawned child #{tracee}: #{opts[:exec].inspect[1..-2]}"

      if (secs = opts[:wait]) > 0
        STDERR.puts "*** waiting #{secs} seconds for child to boot up"
        sleep secs
      end

    elsif opts[:pid].size <= 1
      tracee = opts[:pid].first

    else
      tracers = []

      opts[:pid].each do |pid|
        if child = fork
          tracers << child
        else
          Process.setpgrp
          STDIN.reopen '/dev/null'
          $0 = "rbtrace -p #{pid} (parent: #{Process.ppid})"

          opts[:output] += ".#{pid}" if opts[:output]
          tracee = pid

          # fall through and start tracing
          break
        end
      end

      if tracee.nil?
        # this is the parent
        while true
          begin
            break if tracers.empty?
            if pid = Process.wait
              tracers.delete(pid)
            end
          rescue Interrupt, SignalException
            STDERR.puts "*** waiting on child tracers: #{tracers.inspect}"
            tracers.each do |pid1|
              begin
                Process.kill 'INT', pid1
              rescue Errno::ESRCH
              end
            end
          end
        end

        exit!
      end
    end

    if out = opts[:output]
      output = File.open(out, opts[:append] ? 'a+' : 'w')
      output.sync = true
    end

    begin
      begin
        self.tracer = RBTracer.new(tracee)
      rescue ArgumentError => e
        parser.die :pid, "(#{e.message})"
      end

      if opts[:timeout] > 0
        tracer.timeout = opts[:timeout]
      end

      if opts[:fork_given]
        pid = tracer.fork
        STDERR.puts "*** forked off a busy looping copy at #{pid} (make sure to kill -9 it when you're done)"

      elsif opts[:backtrace_given]
        num = opts[:backtrace]
        code = "caller.first(#{num}).join('|')"

        if res = tracer.eval(code)
          tracer.puts res[1..-2].split('|').join("\n  ")
        end

      elsif opts[:backtraces_given]
        num = opts[:backtraces].to_i
        num = -1 if num == 0

        delim = "146621c9d681409aa"

        code = "Thread.list.reject { |t| t.name == '__RBTrace__' }.map{ |t| t.backtrace[0...#{num}].join(\"#{delim}\")}.join(\"#{delim*2}\")"

        if res = tracer.eval(code)
          tracer.puts res.split(delim).join("\n")
        end

      elsif opts[:memory_given]
        memory_report = File.expand_path('../memory_report.rb', __FILE__)

        require 'tempfile'
        output = Tempfile.new("output")
        output.close

        begin
          code = "Thread.new do; begin; output = '#{output.path}'; eval(File.read('#{memory_report}')); end; end"
          tracer.eval(code)

          File.open(output.path, 'r') do |f|
            while true
              begin
                unless line = f.readline
                  sleep 0.1
                  next
                end

                if line.strip == "__END__"
                  break
                else
                  print line
                end
              rescue EOFError
                sleep 0.1
              end
            end
          end
        ensure
          output.unlink
        end

      elsif opts[:heapdump_given]
        filename = opts[:heapdump]

        if filename == "AUTO"
          require 'tempfile'
          temp = Tempfile.new("dump")
          filename = temp.path
          temp.close
          temp.unlink
        end

        tracer.eval(<<-RUBY)
          Thread.new do
            Thread.current.name = '__RBTrace__'
            pid = ::Process.fork do
              file = File.open('#{filename}.tmp', 'w')
              ObjectSpace.dump_all(output: file)
              file.close
              File.rename('#{filename}.tmp', '#{filename}')
              exit!(0)
            end
            Process.waitpid(pid)
          end
        RUBY
        puts "Heapdump being written to #{filename}"

      elsif opts[:shapesdump_given]
        filename = opts[:shapesdump]

        if filename == "AUTO"
          require 'tempfile'
          temp = Tempfile.new("dump")
          filename = temp.path
          temp.close
          temp.unlink
        end

        tracer.eval(<<-RUBY)
          Thread.new do
            Thread.current.name = '__RBTrace__'
            pid = ::Process.fork do
              file = File.open('#{filename}.tmp', 'w')
              ObjectSpace.dump_shapes(output: file)
              file.close
              File.rename('#{filename}.tmp', '#{filename}')
              exit!(0)
            end
            Process.waitpid(pid)
          end
        RUBY
        puts "Shapes dump being written to #{filename}"

      elsif opts[:eval_given]
        if res = tracer.eval(code = opts[:eval])
          tracer.puts ">> #{code}"
          tracer.puts "=> #{res}"
        end

      elsif opts[:interactive_given]
        require "rbtrace/interactive/#{opts[:interactive]}"

        bin = Gem::Specification.find do |spec|
          bin_file = spec.bin_file(opts[:interactive])
          break bin_file if File.exist?(bin_file)
        end || ENV['PATH'].split(':').find do |path|
          found = Dir["#{path}/*"].find do |file|
            break file if File.basename(file) == opts[:interactive]
          end
          break found if found
        end

        load(bin)

      else
        tracer.out = output if output
        tracer.timeout = opts[:timeout] if opts[:timeout] > 0
        tracer.prefix = ' ' * opts[:prefix]
        tracer.show_time = opts[:start_time]
        tracer.show_duration = !opts[:no_duration]

        tracer.devmode if opts[:devmode_given]
        tracer.gc if opts[:gc_given]

        if opts[:firehose_given]
          tracer.firehose
        else
          tracer.add(methods)       if methods.any?
          if opts[:slow_given] || opts[:slowcpu_given]
            tracer.watch(opts[:slowcpu_given] ? opts[:slowcpu] : opts[:slow], opts[:slowcpu_given])
            tracer.add_slow(smethods) if smethods.any?
          end
        end
        begin
          tracer.recv_loop
        rescue Interrupt, SignalException
        end
      end
    ensure
      if tracer
        tracer.detach
      end

      if opts[:exec_given]
        STDERR.puts "*** waiting on spawned child #{tracee}"
        Process.kill 'TERM', tracee
        Process.waitpid(tracee)
      end
    end
  end
end
