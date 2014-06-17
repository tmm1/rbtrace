require File.expand_path('../lib/rbtrace/version', __FILE__)

Gem::Specification.new do |s|
  s.name = 'rbtrace'
  s.version = RBTracer::VERSION
  s.homepage = 'http://github.com/tmm1/rbtrace'

  s.authors = 'Aman Gupta'
  s.email   = 'aman@tmm1.net'

  s.files = `git ls-files`.split("\n")
  s.extensions = 'ext/extconf.rb'

  s.bindir = 'bin'
  s.executables << 'rbtrace'

  s.add_dependency 'ffi',     '>= 1.0.6'
  s.add_dependency 'trollop', '>= 1.16.2'
  s.add_dependency 'msgpack', '>= 0.4.3'

  s.license = "MIT"
  s.summary = 'rbtrace: like strace but for ruby code'
  s.description = 'rbtrace shows you method calls happening inside another ruby process in real time.'
end
