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
