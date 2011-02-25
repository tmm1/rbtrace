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

unless File.exists?("#{CWD}/dst/lib/libmsgpackc.a")
  Logging.message "Building msgpack\n"

  msgpack = File.basename('msgpack-0.5.4.tar.gz')
  dir = File.basename(msgpack, '.tar.gz')

  Dir.chdir('src') do
    FileUtils.rm_rf(dir) if File.exists?(dir)

    sys("tar zxvf #{msgpack}")
    Dir.chdir(dir) do
      sys("./configure --disable-shared --with-pic --prefix=#{CWD}/dst/")
      sys("make install")
    end
  end
end

FileUtils.cp "#{CWD}/dst/lib/libmsgpackc.a", "#{CWD}/libmsgpackc_ext.a"
$INCFLAGS[0,0] = "-I#{CWD}/dst/include "

unless have_library('msgpackc_ext') and have_header('msgpack.h')
  raise 'msgpack build failed'
end

have_func('rb_during_gc', 'ruby.h')

# increase message size on linux
if RUBY_PLATFORM =~ /linux/
  $defs.push("-DBUF_SIZE=256")
end

# work around the fact that 1.9.1 is fucked, because fuck.
if RUBY_VERSION =~ /^1\.9\.[01]$/
  $defs.push("-DRB_EVENT_HOOKS_HAVE_CALLBACK_DATA=1")
end

# warnings save lives
$CFLAGS << " -Wall "

create_makefile('rbtrace')
