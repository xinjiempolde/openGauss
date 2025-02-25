#!/bin/bash
# 功能：将该目录下的所有*.proto文件进行编译，生成*.cc和*.h文件

# 检查protoc可执行程序是否存在
if command -v protoc > /dev/null 2>&1; then
  echo "find protoc executable, will parse *.proto into *.cc and *.h"
  protoc *.proto --cpp_out=./
else
  echo "cannot find protoc executable"
  exit 1
fi