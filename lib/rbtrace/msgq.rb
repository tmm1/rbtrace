require 'ffi'

module MsgQ
  extend FFI::Library
  ffi_lib FFI::CURRENT_PROCESS

  class EventMsg < FFI::Struct
    BUF_SIZE = RUBY_PLATFORM =~ /linux/ ? 256 : 120
    IPC_NOWAIT = 004000

    layout :mtype, :long,
           :buf, [:char, BUF_SIZE]

    def self.send_cmd(q, str)
      msg = EventMsg.new
      msg[:mtype] = 1
      msg[:buf].to_ptr.put_string(0, str)

      ret = MsgQ.msgsnd(q, msg, BUF_SIZE, 0)
      FFI::LastError.raise if ret == -1
    end

    def self.recv_cmd(q, block=true)
      MsgQ.rb_enable_interrupt if RUBY_VERSION > '1.9' && RUBY_VERSION < '2.0'

      msg = EventMsg.new
      ret = MsgQ.msgrcv(q, msg, BUF_SIZE, 0, block ? 0 : IPC_NOWAIT)
      if ret == -1
        if !block and [Errno::EAGAIN, Errno::ENOMSG].include?(FFI::LastError.exception)
          return nil
        end

        FFI::LastError.raise
      end

      msg[:buf].to_ptr.read_string_length(BUF_SIZE)
    ensure
      MsgQ.rb_disable_interrupt if RUBY_VERSION > '1.9' && RUBY_VERSION < '2.0'
    end
  end

  attach_function :msgget, [:int, :int], :int
  attach_function :msgrcv, [:int, EventMsg.ptr, :size_t, :long, :int], :int
  attach_function :msgsnd, [:int, EventMsg.ptr, :size_t, :int], :int

  if RUBY_VERSION > '1.9' && RUBY_VERSION < '2.0'
    attach_function :rb_enable_interrupt,  [], :void
    attach_function :rb_disable_interrupt, [], :void
  end
end
