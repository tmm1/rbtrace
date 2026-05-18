
require 'irb'

class IRB::Context
  def puts(arg=nil)
    RBTraceCLI.tracer.puts(arg)
  end

  def print(arg=nil)
    RBTraceCLI.tracer.print(arg)
  end

  def inspect_last_value(output = +"")
    output << @last_value.to_s
  end
end

class IRB::WorkSpace
  def evaluate(statements, file=__FILE__, line=__LINE__)
    RBTraceCLI.tracer.eval(statements)
  end
end
