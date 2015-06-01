require 'socket'
require 'fileutils'
require 'msgpack'
require 'ffi'
require 'rbtrace/core_ext'
require 'rbtrace/msgq'

class RBTracer
  # Public: The Fixnum pid of the traced process.
  attr_reader   :pid

  # Public: The IO where tracing output is written (default: STDOUT).
  attr_accessor :out

  # Public: The timeout before giving up on attaching/detaching to a process.
  attr_accessor :timeout

  # The String prefix used on nested method calls (default: ' ').
  attr_accessor :prefix

  # The Boolean flag for showing how long method calls take (default: true).
  attr_accessor :show_duration

  # The Boolean flag for showing the timestamp when method calls start (default: false).
  attr_accessor :show_time

  # Create a new tracer
  #
  # pid - The String of Fixnum process id
  #
  # Returns a tracer.
  def initialize(pid)
    begin
      raise ArgumentError unless pid
      @pid = pid.to_i
      raise ArgumentError unless @pid > 0
      Process.kill(0, @pid)
    rescue TypeError, ArgumentError
      raise ArgumentError, 'pid required'
    rescue Errno::ESRCH
      raise ArgumentError, 'invalid pid'
    rescue Errno::EPERM
      raise ArgumentError, 'could not signal process, are you running as root?'
    end

    @sock = Socket.new Socket::AF_UNIX, Socket::SOCK_DGRAM, 0
    @sockaddr = Socket.pack_sockaddr_un(socket_path)
    @sock.bind(@sockaddr)
    FileUtils.chmod 0666, socket_path
    at_exit { clean_socket_path }

    5.times do
      signal
      sleep 0.15 # wait for process to create msgqs

      @qo = MsgQ.msgget(-@pid, 0666)

      break if @qo > -1
    end

    if @qo == -1
      raise ArgumentError, 'pid is not listening for messages, did you `require "rbtrace"`'
    end

    @klasses = {}
    @methods = {}
    @tracers = Hash.new{ |h,k|
      h[k] = {
        :query => nil,
        :times => [],
        :names => [],
        :exprs => {},
        :last => false,
        :arglist => false
      }
    }
    @max_nesting = @last_nesting = @nesting = 0
    @last_tracer = nil

    @timeout = 5

    @out = STDOUT
    @out.sync = true
    @prefix = '  '
    @printed_newline = true

    @show_time = false
    @show_duration = true
    @watch_slow = false

    attach
  end

  def socket_path
    "/tmp/rbtrace-#{@pid}.sock"
  end

  def clean_socket_path
    FileUtils.rm(socket_path) if File.exists?(socket_path)
  end

  # Watch for method calls slower than a threshold.
  #
  # msec - The Fixnum threshold in milliseconds
  #
  # Returns nothing.
  def watch(msec, cpu_only=false)
    @watch_slow = true
    send_cmd(cpu_only ? :watchcpu : :watch, msec)
  end

  # Turn on the firehose (show all method calls).
  #
  # Returns nothing.
  def firehose
    send_cmd(:firehose)
  end

  # Turn on dev mode.
  #
  # Returns nothing.
  def devmode
    send_cmd(:devmode)
  end

  # Fork the process and return the copy's pid.
  #
  # Returns a Fixnum pid.
  def fork
    send_cmd(:fork)
    if wait('for fork', 30){ !!@forked_pid }
      @forked_pid
    else
      STDERR.puts '*** timed out waiting for fork'
    end
  end

  # Evaluate some ruby code.
  #
  # Returns the String result.
  def eval(code)
    if (err = valid_syntax?(code)) != true
      raise ArgumentError, "#{err.class} for expression #{code.inspect}"
    end

    send_cmd(:eval, code)

    if wait('for eval response', timeout){ !!@eval_result }
      res = @eval_result
      @eval_result = nil
      res
    else
      STDERR.puts '*** timed out waiting for eval response'
    end
  end

  # Turn on GC tracing.
  #
  # Returns nothing.
  def gc
    send_cmd(:gc)
  end

  # Restrict slow tracing to a specific list of methods.
  #
  # methods - The String or Array of method selectors.
  #
  # Returns nothing.
  def add_slow(methods)
    add(methods, true)
  end

  # Add tracers for the given list of methods.
  #
  # methods - The String or Array of method selectors to trace.
  #
  # Returns nothing.
  def add(methods, slow=false)
    Array(methods).each do |func|
      func = func.strip
      next if func.empty?

      if func =~ /^(.+?)\((.+)\)$/
        name, args = $1, $2
        args = args.split(',').map{ |a| a.strip }
      end

      send_cmd(:add, name || func, slow)

      if args and args.any?
        args.each do |arg|
          if (err = valid_syntax?(arg)) != true
            raise ArgumentError, "#{err.class} for expression #{arg.inspect} in method #{func.inspect}"
          end
          if arg =~ /^@/ and arg !~ /^@[_a-z][_a-z0-9]+$/i
            # arg[0]=='@' means ivar, but if this is an expr
            # we can hack a space in front so it gets eval'd instead
            arg = " #{arg}"
          end
          send_cmd(:addexpr, arg)
        end
      end
    end
  end

  # Attach to the process.
  #
  # Returns nothing.
  def attach
    send_cmd(:attach, Process.pid)
    if wait('to attach'){ @attached == true }
      STDERR.puts "*** attached to process #{pid}"
    else
      raise ArgumentError, 'process already being traced?'
    end
  end

  # Detach from the traced process.
  #
  # Returns nothing.
  def detach
    begin
      send_cmd(:detach)
    rescue Errno::ESRCH
    end

    newline

    if wait('to detach cleanly'){ @attached == false }
      newline
      STDERR.puts "*** detached from process #{pid}"
    else
      newline
      STDERR.puts "*** could not detach cleanly from process #{pid}"
    end
  rescue Errno::EINVAL, Errno::EIDRM
    newline
    STDERR.puts "*** process #{pid} is gone"
    # STDERR.puts "*** #{$!.inspect}"
    # STDERR.puts $!.backtrace.join("\n  ")
  rescue Interrupt, SignalException
    retry
  ensure
    clean_socket_path
  end

  # Process events from the traced process.
  #
  # Returns nothing
  def recv_loop
    while true
      ready = IO.select([@sock], nil, nil, 1)

      if ready
        # block until a message arrives
        process_line(recv_cmd)
        # process any remaining messages
        recv_lines
      else
        Process.kill(0, @pid)
      end

    end
  rescue Errno::EINVAL, Errno::EIDRM, Errno::ESRCH
    # process went away
  end

  # Process events from the traced process, without blocking if
  # there is nothing to do. This is a useful way to drain the buffer
  # so messages do not accumulate in kernel land.
  #
  # Returns nothing.
  def recv_lines
    50.times do
      break unless line = recv_cmd(false)
      process_line(line)
    end
  end

  def puts(arg=nil)
    @printed_newline = true
    arg ? @out.puts(arg) : @out.puts
  end

  private

  def signal
    Process.kill 'URG', @pid
  end

  # Process incoming events until either a timeout or a condition becomes true.
  #
  # time - The Fixnum timeout in seconds.
  # block - The Block that is checked every 50ms until it returns true.
  #
  # Returns true when the condition was met, or false on a timeout.
  def wait(reason, time=@timeout)
    wait = 0.05 # polling interval

    (time/wait).to_i.times do
      begin
        recv_lines
        sleep(wait)
        begin
          signal
        rescue Errno::ESRCH
          break
        end
        time -= wait

        return true if yield
      rescue Interrupt
        STDERR.puts "*** waiting #{reason} (#{time.to_i}s left)"
        retry
      end
    end

    false
  end

  def send_cmd(*cmd)
    begin
      msg = cmd.to_msgpack
      raise ArgumentError, 'command is too long' if msg.bytesize > MsgQ::EventMsg::BUF_SIZE
      MsgQ::EventMsg.send_cmd(@qo, msg)
    rescue Errno::EINTR
      retry
    end
    signal
    recv_lines
  end

  def recv_cmd(block=true)
    if block
      @sock.recv(65536)
    else
      @sock.recv_nonblock(65536)
    end
  rescue Errno::EAGAIN
    nil
  end

  def valid_syntax?(code)
    begin
      Kernel.eval("#{code}\nBEGIN {return true}", nil, 'rbtrace_expression', 0)
    rescue Exception => e
      e
    end
  end

  def print(arg)
    @printed_newline = false
    @out.print(arg)
  end

  def newline
    puts unless @printed_newline
    @printed_newline = true
  end

  def parse_cmd(line)
    unpacker = MessagePack::Unpacker.new
    unpacker.feed(line)

    obj = nil
    unpacker.each{|o| obj = o; break }
    obj
  end

  def process_line(line)
    return unless cmd = parse_cmd(line)
    event = cmd.shift

    case event
    when 'during_gc'
      sleep 0.01
      signal
      return

    when 'attached'
      tracer_pid, = *cmd
      if tracer_pid != Process.pid
        STDERR.puts "*** process #{pid} is already being traced (#{tracer_pid} != #{Process.pid})"
        exit!(-1)
      end

      @attached = true
      return

    when 'detached'
      tracer_pid, = *cmd
      if tracer_pid != Process.pid
        STDERR.puts "*** process #{pid} detached #{tracer_pid}, but we are #{Process.pid}"
      else
        @attached = false
      end

      return
    end

    unless @attached
      STDERR.puts "*** got #{event} before attaching"
      return
    end

    case event
    when 'forked'
      pid, = *cmd
      @forked_pid = pid

    when 'evaled'
      res, = *cmd
      @eval_result = res

    when 'mid'
      mid, name = *cmd
      @methods[mid] = name

    when 'klass'
      kid, name = *cmd
      @klasses[kid] = name

    when 'add'
      tracer_id, query = *cmd
      if tracer_id == -1
        STDERR.puts "*** unable to add tracer for #{query}"
      else
        @tracers.delete(tracer_id)
        @tracers[tracer_id][:query] = query
      end

    when 'newexpr'
      tracer_id, expr_id, expr = *cmd
      tracer = @tracers[tracer_id]

      if expr_id > -1
        tracer[:exprs][expr_id] = expr.strip
      end

    when 'exprval'
      tracer_id, expr_id, val = *cmd

      tracer = @tracers[tracer_id]
      expr = tracer[:exprs][expr_id]

      if tracer[:arglist]
        print ', '
      else
        print '('
      end

      print "#{expr}="
      print val
      tracer[:arglist] = true

    when 'call','ccall'
      time, tracer_id, mid, is_singleton, klass = *cmd

      tracer = @tracers[tracer_id]
      klass = @klasses[klass]
      name = klass ? "#{klass}#{ is_singleton ? '.' : '#' }" : ''
      name += @methods[mid] || '(unknown)'

      tracer[:times] << time
      tracer[:names] << name

      if @last_tracer and @last_tracer[:arglist]
        print ')'
        @last_tracer[:arglist] = false
      end
      newline
      if @show_time
        t = Time.at(time/1_000_000)
        print t.strftime("%H:%M:%S.")
        print "%06d " % (time - t.to_f*1_000_000).round
      end
      print @prefix*@nesting if @nesting > 0
      print name

      @nesting += 1
      @max_nesting = @nesting if @nesting > @max_nesting
      @last_nesting = @nesting
      @last_tracer = tracer
      tracer[:last] = "#{name}:#{@nesting-1}"

    when 'return','creturn'
      time, tracer_id = *cmd
      tracer = @tracers[tracer_id]

      @nesting -= 1 if @nesting > 0

      if start = tracer[:times].pop
        name = tracer[:names].pop
        diff = time - start
        @last_tracer[:arglist] = false if @last_tracer and @last_tracer[:last] != "#{name}:#{@nesting}"

        print ')' if @last_tracer and @last_tracer[:arglist]

        unless tracer == @last_tracer and @last_tracer[:last] == "#{name}:#{@nesting}"
          newline
          print ' '*16 if @show_time
          print @prefix*@nesting if @nesting > 0
          print name
        end
        print ' <%f>' % (diff/1_000_000.0) if @show_duration
        newline

        if @nesting == 0 and @max_nesting > 1
          # unless tracer == @last_tracer and @last_tracer[:last] == name
            puts
          # end
        end
      end

      tracer[:arglist] = false
      @last_nesting = @nesting

    when 'slow', 'cslow'
      time, diff, nesting, mid, is_singleton, klass = *cmd

      klass = @klasses[klass]
      name = klass ? "#{klass}#{ is_singleton ? '.' : '#' }" : ''
      name += @methods[mid] || '(unknown)'

      newline
      nesting = @nesting if @nesting > 0

      if @show_time
        t = Time.at(time/1_000_000)
        print t.strftime("%H:%M:%S.")
        print "%06d " % (time - t.to_f*1_000_000).round
      end

      print @prefix*nesting if nesting > 0
      print name
      if @show_duration
        print ' '
        print "<%f>" % (diff/1_000_000.0)
      end
      puts
      puts if nesting == 0 and @max_nesting > 1

      @max_nesting = nesting if nesting > @max_nesting
      @last_nesting = nesting

    when 'gc_start'
      time, = *cmd
      @gc_start = time
      print 'garbage_collect'

    when 'gc_end'
      time, = *cmd
      diff = time - @gc_start
      # if @gc_mark
      #   mark = ((@gc_mark - @gc_start) * 100.0 / diff).to_i
      #   print '(mark: %d%%, sweep: %d%%)' % [mark, 100-mark]
      # end
      print ' <%f>' % (diff/1_000_000.0) if @show_duration
      @gc_start = nil
      newline

    when 'gc'
      time, = *cmd
      @gc_mark = time

      unless @gc_start
        newline
        if @show_time
          t = Time.at(time/1_000_000)
          print t.strftime("%H:%M:%S.")
          print "%06d " % (time - t.to_f*1_000_000).round
        end
        print @prefix*@last_nesting if @last_nesting > 0
        print "garbage_collect"
        puts if @watch_slow
      end

    else
      puts "unknown event #{event}: #{cmd.inspect}"

    end
  rescue => e
    STDERR.puts "error on #{event}: #{cmd.inspect}"
    raise e
  end

end
