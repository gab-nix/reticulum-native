# reticulum-c

A portable C17 implementation of the Reticulum networking stack and LXMF client.
The project is under active development; protocol interoperability is validated
against the reference Python implementations.

## Requirements

- A C17 compiler (Clang or GCC)
- CMake 3.21 or newer
- OpenSSL 3.x development headers and libraries
- POSIX threads (macOS and Linux)

## Build and test

```sh
cmake -S . -B build -G Ninja -DRETICULUM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

To run with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake -S . -B build-sanitize -G Ninja \
  -DRETICULUM_BUILD_TESTS=ON \
  -DRETICULUM_SANITIZERS=address,undefined
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
```

Set `RETICULUM_WARNINGS_AS_ERRORS=ON` for strict local and CI builds.

