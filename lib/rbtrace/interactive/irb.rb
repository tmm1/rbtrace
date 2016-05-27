
require 'irb'

class IRB::Context
  def puts(arg=nil)
    RBTraceCLI.tracer.puts(arg)
  end

  def print(arg=nil)
    RBTraceCLI.tracer.print(arg)
  end

  def inspect_last_value
    @last_value
  end
end

class IRB::WorkSpace
  def evaluate(context, statements, file=__FILE__, line=__LINE__)
    RBTraceCLI.tracer.eval(statements)
  end
end
