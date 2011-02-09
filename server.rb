puts Process.pid
require 'ext/rbtrace'

while true
  Dir.chdir("/tmp") do
    sleep rand*0.5
  end
end
