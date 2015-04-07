
require 'irb'

class IRB::Context
  def puts s=nil
    RBTraceCLI.tracer.puts(s)
  end

  def print s=nil
    RBTraceCLI.tracer.print(s)
  end

  def inspect_last_value
    @last_value
  end
end

class IRB::WorkSpace
  def evaluate(context, statements, file = __FILE__, line = __LINE__)
    RBTraceCLI.tracer.eval(statements)
  end
end
