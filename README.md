# Fiona

Fiona is a coroutine runtime library built on top of io_uring and C++20's
coroutines. This means that Fiona is a Linux-only library.

Fiona includes a vcpkg.json for easy dependency management.

## Useful Scripts for Developers

### Testing

run.sh

```bash
#!/bin/bash

set -e

clear

ulimit -n 25000

ninja -C __build__ fiona_verify_interface_header_sets

ctest \
  --test-dir __build__/ \
  -j20 \
  --output-on-failure \
  --schedule-random \
  "$@"
```

toolchain.cmake
```cmake
include(/home/exbigboss/cpp/vcpkg/scripts/buildsystems/vcpkg.cmake)
list(APPEND CMAKE_PREFIX_PATH /home/exbigboss/cpp/__install__)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS_INIT "-Wall -Wextra -pedantic -fsanitize=address,undefined")
```

For emulating a CI setup locally, use something like:
```bash
#!/bin/bash

set -e

clean() {
  input=$1
  str=${input//[^a-zA-Z0-9]/}
  echo "$str"
}

build_and_test() {
  local cc
  cc=$(clean "$1")

  local build_type
  build_type=$(clean "$2")

  local build_dir
  local cxx_flags_init

  if [ -z "$3" ]
  then
    build_dir=__ci_build__/${cc}/${build_type}/nosan
    cxx_flags_init="-Wall -Wextra -pedantic -Werror"
  else
    local sanitizers
    sanitizers=$(clean "$3")
    build_dir=__ci_build__/${cc}/${build_type}/${sanitizers}
    cxx_flags_init="-Wall -Wextra -pedantic -Werror -fsanitize=$3"
  fi

  mkdir -p "$build_dir"

  cmake \
    --no-warn-unused-cli \
    -DBUILD_TESTING=ON \
    -DFIONA_BUILD_TESTING=ON \
    -DCMAKE_PREFIX_PATH=/home/exbigboss/cpp/__install__ \
    -DCMAKE_BUILD_TYPE="$2" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE \
    -DCMAKE_CXX_COMPILER="$1" \
    -DCMAKE_CXX_STANDARD=20 \
    -DCMAKE_CXX_EXTENSIONS=OFF \
    -DCMAKE_CXX_FLAGS_INIT="$cxx_flags_init" \
    -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
    -DCMAKE_TOOLCHAIN_FILE=/home/exbigboss/cpp/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -S/home/exbigboss/cpp/fiona \
    -B/home/exbigboss/cpp/fiona/"$build_dir" \
    -G Ninja

  cmake --build /home/exbigboss/cpp/fiona/"$build_dir" --config "$2" --target all

  ulimit -n 25000

  ninja -C "$build_dir" fiona_verify_interface_header_sets

  ctest \
    --test-dir "$build_dir" \
    -j20 \
    --output-on-failure \
    --schedule-random \
    "$@"
}

clear

rm -r __ci_build__

build_and_test "clang++-17" "debug" "address,undefined"
build_and_test "clang++-17" "release" "address,undefined"
build_and_test "clang++-17" "release" "thread"
build_and_test "clang++-17" "release"

build_and_test "clang++-16" "debug" "address,undefined"
build_and_test "clang++-16" "release" "address,undefined"
build_and_test "clang++-16" "release" "thread"
build_and_test "clang++-16" "release"

build_and_test "clang++-15" "debug" "address,undefined"
build_and_test "clang++-15" "release" "address,undefined"
build_and_test "clang++-15" "release" "thread"
build_and_test "clang++-15" "release"

build_and_test "g++-13" "debug" "address,undefined"
build_and_test "g++-13" "release" "address,undefined"
build_and_test "g++-13" "release" "thread"
build_and_test "g++-13" "release"

build_and_test "g++-12" "debug" "address,undefined"
build_and_test "g++-12" "release" "address,undefined"
build_and_test "g++-12" "release" "thread"
build_and_test "g++-12" "release"
```
