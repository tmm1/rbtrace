# Converts the output of rbtrace to a different format
class RBTrace::Converter
  # Public: The IO where converter input is read.
  attr_accessor :input

  # Public: The IO where converter output is written (default: STDOUT).
  attr_accessor :output

  # Build a converter of the given type and input
  #
  # type - The Symbol type of the converter to build.
  # input - The IO where converter input is read.
  #
  # Returns a converter
  def self.build(type, input)
    case type.to_sym
    when :flamegraph
      Flamegraph.new(input)
    else
      raise "Unknown type: #{type}"
    end
  end

  # Creates a new converter
  #
  # input - The IO where converter input is read.
  #
  # Returns a converter
  def initialize(input)
    @input = input
    @output = $stdout
  end

  # Converts the input to a format suitable for the flamegraph tool,
  # e.g. https://github.com/brendangregg/FlameGraph or https://www.speedscope.app/
  class Flamegraph < RBTrace::Converter

    # Converts and writes the output to the output IO.
    # Returns nothing.
    def convert
      stack = []

      input.each_line do |line|
        match = line =~ /\S/
        next if match.nil?

        # Calculate depth by the number of leading spaces (each indent is 2 spaces in the example)
        depth = match / 2
        method_line = line.strip

        matches = method_line.match(/(.*) <\s*(.*)>$/)
        method =
          if matches
            method_name, timing = matches[1], matches[2]
            timing = (timing.to_f * 1000000).to_i # int microseconds
            [method_name.strip, timing]
          else
            [method_line]
          end

        if method.size == 2 && stack.length == depth # on the same depth
          output.puts (stack + ["#{method[0]} #{method[1]}"]).join(';')
          next
        end

        if method.size == 1 # start of new block
          stack.push(method)
          next
        end

        if stack.length > depth # go to lesser level
          stack.pop
        end
      end
    end
  end
end
