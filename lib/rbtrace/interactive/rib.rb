
require 'rib/config'

module Rib::Rbtrace
  def puts s=nil
    RBTraceCLI.tracer.puts(s)
  end

  def print s=nil
    RBTraceCLI.tracer.print(s)
  end

  def format_result result
    result_prompt + result
  end

  def loop_eval input
    RBTraceCLI.tracer.eval(input)
  end
end

Rib::Shell.send(:include, Rib::Rbtrace)
