class Test
  def call
    self[:a] = :b
  end
  def []=(k,v)
    Another.new.call
    :ok
  end
end

class Another
  def call
    self[:a] = :b
  end
  def []=(key, value)
    sup = 123
  end
end

module Do
  module It
    def something
    end
  end
end

class Slow
  include Do::It
  def self.something
    sleep 0.01
  end
end

require 'rbtrace'
rbtrace 'Slow.something'
rbtrace 'Do::It#something'
rbtrace '[]='
rbtrace 'call'

1.times do
  Slow.something
  Slow.new.something
  Test.new.call
end

__END__

Slow.something <10047>
Do::It#something <4>
Test#call
   Test#[]=
      Another#call
         Another#[]=          <4>
      <14>
   <28>
<40>

