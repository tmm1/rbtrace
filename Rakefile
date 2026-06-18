require "bundler/gem_tasks"

CLEAN.include("ext/Makefile", "ext/*.o", "ext/*.so", "ext/*.bundle", "ext/mkmf.log")

desc "Compile the c extension"
task :compile do
  if File.exist?("ext/Makefile")
    sh "cd ext && make clean"
  end
  sh "cd ext && ruby extconf.rb"
  sh "cd ext && make"
end

desc "Run the integration test suite"
task :test do
  sh "./test.sh"
end

task :default => :test

task :build => :compile
