output
fork do
  file = File.new(output, 'w')

  file.puts "GC Stats",""
  GC.stat.each do |k, v|
    file.puts "#{k}: #{v}"
  end

  file.puts "", "Object Stats", ""
  require 'objspace'
  ObjectSpace.count_objects.sort{|a,b| b[1] <=> a[1]}.each do |k, v|
    file.puts "#{k}: #{v}"
  end


  file.puts "__END__"
  file.flush
  file.close
end
