#!/bin/bash
echo "参数数量: $#"
for arg in "$@"; do
    addr2line -e ./build/*.elf "$arg"
done
