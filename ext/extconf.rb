CWD = File.expand_path('../', __FILE__)

def sys(cmd)
  puts "  -- #{cmd}"
  unless ret = xsystem(cmd)
    raise "#{cmd} failed, please report to https://github.com/tmm1/rbtrace/issues"
  end
  ret
end

require 'mkmf'
require 'fileutils'

unless have_library('msgpackc') and have_header('msgpack.h')
  raise 'msgpack build failed'
end

have_func('rb_during_gc', 'ruby.h')
have_func('rb_gc_add_event_hook', ['ruby.h', 'node.h'])
have_func('rb_postponed_job_register_one', 'ruby.h')

# increase message size on linux
if RUBY_PLATFORM =~ /linux/
  $defs.push("-DBUF_SIZE=256")
end

# warnings save lives
$CFLAGS << " -Wall "

create_makefile('rbtrace')
