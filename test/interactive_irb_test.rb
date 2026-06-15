# frozen_string_literal: true
#
# behavioral regression test for `rbtrace --interactive irb` (see PR #108).
#
# evaluating an expression should print its value back to the terminal; we had
# a bug that made it print nothing. The bug only surfaces on a TTY.
#
# usage: ruby test/interactive_irb_test.rb <pid-to-trace>

require "pty"
require "timeout"

pid = Integer(ARGV.fetch(0))
cmd = "bundle exec ./bin/rbtrace -p #{pid} --interactive irb"

output = +""
printed_result = false

begin
  PTY.spawn({ "TERM" => "xterm" }, cmd) do |reader, writer, _child_pid|
    Timeout.timeout(30) do
      output << reader.readpartial(4096) until output.include?(">") # wait for prompt
      writer.puts "1 + 1"
      output << reader.readpartial(4096) until output.include?("=> 2")
      printed_result = true
      writer.puts "exit"
    end
  end
rescue Timeout::Error, Errno::EIO, EOFError
  # printed_result stays false unless we saw "=> 2" above
end

if printed_result
  puts "PASS: `--interactive irb` printed the eval result"
else
  warn "FAIL: `--interactive irb` did not print `=> 2`"
  warn output.gsub(/\e\[[0-9;?]*[A-Za-z]/, "")
  exit 1
end
