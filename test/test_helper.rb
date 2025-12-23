# encoding: UTF-8

ext_path = File.expand_path(File.join(__dir__, '..', 'ext'))
$LOAD_PATH.unshift(File.expand_path(ext_path))

require 'rbtrace'
require 'rbtrace/cli'
require 'minitest/autorun'

class TestCase < Minitest::Test
end
