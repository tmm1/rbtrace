# frozen_string_literal: true

module RBTrace
  class << self
    def eval_and_inspect(code)
      t = Thread.new do
        Thread.current.name = '__RBTrace__'
        Thread.current[:output] = eval_context.eval(code).inspect
      end
      t.join
      t[:output]
    end

    private

    def eval_context
      @eval_context ||= binding
    end
  end
end

require 'rbtrace.so'
