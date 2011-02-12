require 'ext/rbtrace'

while true
  proc {
    Dir.chdir("/tmp") do
      Dir.pwd
      Process.pid
      'hi'.gsub('hi'){ |match| match*2 }
      sleep rand*0.5
    end
  }.call
end
