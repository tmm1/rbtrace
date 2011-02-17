Gem::Specification.new do |s|
  s.name = 'rbtrace'
  s.version = '0.2.7'
  s.homepage = 'http://github.com/tmm1/rbtrace'

  s.authors = 'Aman Gupta'
  s.email   = 'aman@tmm1.net'

  s.files = `git ls-files`.split("\n")
  s.extensions = 'ext/extconf.rb'

  s.bindir = 'bin'
  s.executables << 'rbtrace'

  s.add_dependency 'ffi', '>= 1.0.5'
  s.add_dependency 'trollop', '>= 1.16.2'

  s.summary = 'rbtrace: like strace but for ruby code'
  s.description = 'rbtrace shows you method calls happening inside another ruby process in real time.'
end
