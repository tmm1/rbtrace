# frozen_string_literal: true

require File.expand_path('../test_helper', __FILE__)
require 'stringio'

class ConverterFlamegraphTest < TestCase
  def convert(input)
    output = StringIO.new
    converter = RBTrace::Converter::Flamegraph.new(StringIO.new(input))
    converter.output = output
    converter.convert

    output.rewind

    output.read
  end

  def test_convert_flamegraph
    input = <<~INPUT
IO.select <0.000044>
IO.select <0.000055>

Puma::Client#eagerly_finish
  IO.select <0.000010>
  Puma::Client#try_to_finish
    BasicSocket#read_nonblock
      BasicSocket#__read_nonblock <0.000018>
    BasicSocket#read_nonblock <0.000029>
  Puma::Client#try_to_finish <0.000153>
Puma::Client#eagerly_finish <0.000179>
Puma::ThreadPool#<<
  Thread::Mutex#synchronize
    Thread::ConditionVariable#signal <0.000008>
  Thread::Mutex#synchronize <0.000027>
Puma::ThreadPool#<< <0.000035>
    INPUT

    expected_output = <<~OUTPUT
IO.select 44
IO.select 55
Puma::Client#eagerly_finish;IO.select 10
Puma::Client#eagerly_finish;Puma::Client#try_to_finish;BasicSocket#read_nonblock;BasicSocket#__read_nonblock 18
Puma::ThreadPool#<<;Thread::Mutex#synchronize;Thread::ConditionVariable#signal 8
    OUTPUT

    assert_equal expected_output, convert(input)
  end
end
