#!/bin/sh
set -eu
arrow_dir=${ARROW_DIR:-$(python3 -c 'from importlib.util import find_spec; from pathlib import Path; print(Path(find_spec("pyarrow").origin).parent)')}
exec g++ -std=c++20 -O3 -DNDEBUG -pthread -I"$arrow_dir/include" main.cpp \
    -L"$arrow_dir" -Wl,-rpath-link,"$arrow_dir" '-Wl,-rpath,$ORIGIN/lib' \
    -l:libparquet.so.1900 -l:libarrow.so.1900 -o "${1:-shopping-sessions}"
