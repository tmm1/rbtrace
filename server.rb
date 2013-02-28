require 'ext/rbtrace'

class String
  def multiply_vowels(num)
    @test = 123
    gsub(/[aeiou]/){ |m| m*num }
  end
end

def fib(n)
  curr = 0
  succ = 1

  n.times{ curr, succ = succ, curr + succ }
  curr
end

(reload_test = proc{
  Object.send(:remove_const, :Test) if defined? Test
  Test = Class.new do
    def self.run
      :abc
    end
  end
}).call

while true
  proc {
    Dir.chdir("/tmp") do
      Dir.pwd
      Process.pid
      'hello'.multiply_vowels(3){ :ohai }
      sleep rand*0.5

      ENV['blah']
      GC.start

      reload_test.call
      Test.run

      #fib(1024*100)
    end
  }.call
end
