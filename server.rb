require 'ext/rbtrace'

while true
  proc {
    Dir.chdir("/tmp") do
      Dir.pwd
      Process.pid
      ("hi"*5).gsub('hi', 'hello')
      sleep rand*0.5
    end
  }.call
end
