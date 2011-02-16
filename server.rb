require 'ext/rbtrace'

class String
  def multiply_vowels(num)
    @test = 123
    gsub(/[aeiou]/){ |m| m*num }
  end
end

while true
  proc {
    Dir.chdir("/tmp") do
      Dir.pwd
      Process.pid
      'hello'.multiply_vowels(3){ :ohai }
      sleep rand*0.5
    end
  }.call
end
