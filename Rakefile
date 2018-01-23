require "bundler/gem_tasks"

desc "Compile the c extension"
task :compile do
  if File.exist?("ext/Makefile")
    system "(cd ext && make clean)"
  end
  system "(cd ext && ruby extconf.rb)"
  system "(cd ext && make)"
end

task :build => :compile


