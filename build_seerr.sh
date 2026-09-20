#!/bin/bash
cd /c/Users/makss/Desktop/scoped/projects/tezeusz
CXX=/c/msys64/mingw64/bin/c++.exe
printf '#include <string>\n#include <cstdlib>\nint main(){ std::string s="x"; return (int)s.size()+atoi("1"); }\n' > /tmp/tinc.cpp
echo "=== A: plain (no extra includes) ==="
$CXX -std=gnu++20 -fsyntax-only /tmp/tinc.cpp 2>&1 && echo "A OK"
echo "=== B: -isystem C:/msys64/mingw64/include ==="
$CXX -std=gnu++20 -isystem C:/msys64/mingw64/include -fsyntax-only /tmp/tinc.cpp 2>&1 && echo "B OK"
echo "=== C: -I/mingw64/include ==="
$CXX -std=gnu++20 -I/mingw64/include -fsyntax-only /tmp/tinc.cpp 2>&1 && echo "C OK"
echo "=== D: both (as ninja has) ==="
$CXX -std=gnu++20 -I/mingw64/include -isystem C:/msys64/mingw64/include -fsyntax-only /tmp/tinc.cpp 2>&1 && echo "D OK"
rm -f /tmp/tinc.cpp
