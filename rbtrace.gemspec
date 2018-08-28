# frozen_string_literal: true

require File.expand_path('../lib/rbtrace/version', __FILE__)

Gem::Specification.new do |s|
  s.name = 'rbtrace'
  s.version = RBTracer::VERSION
  s.homepage = 'http://github.com/tmm1/rbtrace'

  s.authors = 'Aman Gupta'
  s.email   = 'aman@tmm1.net'

  s.require_paths = ['lib', 'ext']

  s.files = `git ls-files`.split("\n")
  s.extensions = 'ext/extconf.rb'

  s.bindir = 'bin'
  s.executables << 'rbtrace'


  s.add_dependency 'ffi',      '>= 1.0.6'
  s.add_dependency 'optimist', '>= 3.0.0'
  s.add_dependency 'msgpack',  '>= 0.4.3'

  s.add_development_dependency "rake", "~> 10.0"

  s.license = "MIT"
  s.summary = 'rbtrace: like strace but for ruby code'
  s.description = 'rbtrace shows you method calls happening inside another ruby process in real time.'
end
