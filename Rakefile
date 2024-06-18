require "bundler/gem_tasks"
require "rake/testtask"

desc "Compile the c extension"
task :compile do
  if File.exist?("ext/Makefile")
    system "(cd ext && make clean)"
  end
  system "(cd ext && ruby extconf.rb)"
  system "(cd ext && make)"
end

task :build => :compile

task :default => :test

desc 'Run the rbtracer test suite'
Rake::TestTask.new do |t|
  t.libs += %w(lib ext test)
  t.test_files = Dir['test/**_test.rb']
  t.verbose = true
  t.warning = true
end
