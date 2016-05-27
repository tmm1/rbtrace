
require 'rib/config'

module Rib::Rbtrace
  def puts(arg=nil)
    RBTraceCLI.tracer.puts(arg)
  end

  def print(arg=nil)
    RBTraceCLI.tracer.print(arg)
  end

  def format_result(result)
    result_prompt + result
  end

  def loop_eval(input)
    RBTraceCLI.tracer.eval(input)
  end
end

Rib::Shell.send(:include, Rib::Rbtrace)
