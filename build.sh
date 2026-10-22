#!/bin/bash
# Cross-builds dinput8.dll (x64) with clang + lld against mingw-w64 headers, no static CRT.
set -e
cd "$(dirname "$0")"
SYS="${SYSROOT:-/usr}"   # prefix containing x86_64-w64-mingw32/include (mingw-w64 headers + import libs)
mkdir -p build out
CXXFLAGS="--target=x86_64-w64-windows-gnu -isystem $SYS/x86_64-w64-mingw32/include -O2 -std=c++17 \
  -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables -ffreestanding -fno-stack-protector \
  -mno-stack-arg-probe -fno-threadsafe-statics -Wall -Wno-unused-function -Wno-missing-braces \
  -Wno-unknown-pragmas -Wno-unused-variable -Wno-unused-but-set-variable -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE"
OBJS=""
for f in src/*.cpp; do
  o=build/$(basename "${f%.cpp}").o
  clang++ $CXXFLAGS -c "$f" -o "$o"
  OBJS="$OBJS $o"
done
ld.lld -m i386pep --shared --entry DllMainCRTStartup --subsystem windows --dynamicbase --nxcompat --high-entropy-va \
  -o out/dinput8.dll $OBJS src/exports.def -L$SYS/lib -L$SYS/x86_64-w64-mingw32/lib -lkernel32 -luser32 -ladvapi32 -lmsvcrt \
  --out-implib build/dinput8.lib
ls -la out/dinput8.dll
