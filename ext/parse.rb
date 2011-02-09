tracers = {}
nesting = 0

file = ARGV[0] ? File.open(ARGV[0]) : STDIN

file.each_line do |line|
  time, event, id, *args = line.strip.split(',')
  time = time.to_i
  id = id.to_i
  tracer = tracers[id]

  case event
  when 'add'
    name = args.first
    tracers[id] = {
      :name => name,
      :times => [],
      :ctimes => []
    }

  when 'remove'
    tracers.delete(id)

  when 'call','ccall'
    method, is_singleton, klass = *args
    is_singleton = (is_singleton == '1')
    bucket = (event == 'call' ? :times : :ctimes)

    tracer[bucket] << time

    if nesting > 0
      puts
      print '   '*nesting
    end
    print klass
    print is_singleton ? '.' : '#'
    print method
    print ' '

    nesting += 1

  when 'return','creturn'
    nesting -= 1 if nesting > 0

    bucket = (event == 'return' ? :times : :ctimes)
    if start = tracer[bucket].pop
      diff = time - start

      print '   '*nesting
      puts "<%f>" % (diff/1_000_000.0)
    end

  end
end
