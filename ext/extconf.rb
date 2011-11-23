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
  cflags, ldflags = ENV['CFLAGS'], ENV['LDFLAGS']

  # build fat binaries on osx
  if RUBY_PLATFORM =~ /darwin/ and (archs = Config::CONFIG['LDFLAGS'].scan(/(-arch\s+.+?)(?:\s|$)/).flatten).any?
    ENV['CFLAGS'] = "#{cflags} #{archs.join(' ')}"
    ENV['LDFLAGS'] = "#{ldflags} #{archs.join(' ')}"
  end

  Dir.chdir('src') do
    FileUtils.rm_rf(dir) if File.exists?(dir)

    sys("tar zxvf #{msgpack}")
    Dir.chdir(dir) do
      if RUBY_PLATFORM =~ /i686/ and gcc = `gcc -v 2>&1` and gcc =~ /gcc version (\d\.\d)/ and $1.to_f <= 4.1
        ENV['CFLAGS'] = " #{ENV['CFLAGS']} -march=i686 "
      end
      sys("./configure --disable-dependency-tracking --disable-shared --disable-cxx --with-pic --prefix=#{CWD}/dst/")
      sys("make install")
    end
  end

  if cflags or ldflags
    ENV['CFLAGS'], ENV['LDFLAGS'] = cflags, ldflags
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

# warnings save lives
$CFLAGS << " -Wall "

create_makefile('rbtrace')
