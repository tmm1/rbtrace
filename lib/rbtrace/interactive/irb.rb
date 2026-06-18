
require 'irb'

class IRB::Context
  def puts(arg=nil)
    RBTraceCLI.tracer.puts(arg)
  end

  def print(arg=nil)
    RBTraceCLI.tracer.print(arg)
  end

  # IRB renders a result by calling this and printing the +output+ buffer it
  # hands in: since 1.15 it streams the inspected value into +output+ rather
  # than using the return value. @last_value is the already-inspected String
  # returned by the remote eval, so we write it straight into the buffer.
  def inspect_last_value(output = +"")
    output << @last_value.to_s
  end
end

class IRB::WorkSpace
  def evaluate(statements, file=__FILE__, line=__LINE__)
    RBTraceCLI.tracer.eval(statements)
  end
end
