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

libdir = File.basename RbConfig::CONFIG['libdir']

unless File.exists?("#{CWD}/dst/#{libdir}/libmsgpackc.a")
  Logging.message "Building msgpack\n"

  msgpack = File.basename('msgpack-1.1.0.tar.gz')
  dir = File.basename(msgpack, '.tar.gz')
  cflags, ldflags = ENV['CFLAGS'], ENV['LDFLAGS']
  cc = ENV['CC']

  # build fat binaries on osx
  if RUBY_PLATFORM =~ /darwin/ and (archs = RbConfig::CONFIG['LDFLAGS'].scan(/(-arch\s+.+?)(?:\s|$)/).flatten).any?
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
      if RUBY_PLATFORM =~ /darwin/ and File.exist?("/usr/bin/gcc-4.2")
        ENV['CC'] = '/usr/bin/gcc-4.2'
      end
      puts "  -- env CFLAGS=#{ENV['CFLAGS'].inspect} LDFLAGS=#{ENV['LDFLAGS'].inspect} CC=#{ENV['CC'].inspect}"
      sys("./configure --disable-dependency-tracking --disable-shared --with-pic --prefix=#{CWD}/dst/ --libdir=#{CWD}/dst/#{libdir}")
      sys("make install")
    end
  end

  if cflags or ldflags
    ENV['CFLAGS'], ENV['LDFLAGS'] = cflags, ldflags
  end
  if cc
    ENV['CC'] = cc
  end
end

FileUtils.cp "#{CWD}/dst/#{libdir}/libmsgpackc.a", "#{CWD}/libmsgpackc_ext.a"
$INCFLAGS[0,0] = "-I#{CWD}/dst/include "

unless have_library('msgpackc_ext') and have_header('msgpack.h')
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
