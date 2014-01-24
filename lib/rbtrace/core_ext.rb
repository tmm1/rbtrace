class String
  alias :bytesize :size
end unless ''.respond_to?(:bytesize)

module FFI::LastError
  Errnos = Errno::constants.map(&Errno.method(:const_get)).inject({}) do |hash, c|
    hash[ c.const_get(:Errno) ] = c
    hash
  end

  def self.exception
    Errnos[error]
  end
  def self.raise(msg=nil)
    Kernel.raise exception, msg
  end
end
