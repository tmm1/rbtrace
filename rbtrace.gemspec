Gem::Specification.new do |s|
  s.name = 'rbtrace'
  s.version = '0.1.0'
  s.homepage = 'http://github.com/tmm1/rbtrace'

  s.authors = "Aman Gupta"
  s.email   = "aman@tmm1.net"

  s.files = `git ls-files`.split("\n")
  s.extensions = "ext/extconf.rb"

  s.bindir = 'bin'
  s.executables << 'rbtrace'

  s.add_dependency 'ffi'

  s.summary = 'rbtrace: like strace but for ruby code'
end
