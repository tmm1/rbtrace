require 'ext/rbtrace'

class String
  def multiply_vowels(num)
    @test = 123
    gsub(/[aeiou]/){ |m| m*num }
  end
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

      GC.start

      reload_test.call
      Test.run
    end
  }.call
end
