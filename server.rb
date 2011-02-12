require 'ext/rbtrace'

while true
  Dir.chdir("/tmp") do
    Dir.pwd
    Process.pid
    sleep rand*0.5
  end
end
