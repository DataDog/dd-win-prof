# Unit tests

C++ tests built with Google Test. CMake fetches the test dependencies; NuGet is
not required. The target compiles profiler implementation files directly into
`Tests.exe` and links the dynamic libdatadog FFI library.

After [configuring CMake](../../README.md#build), run from the repository root:

```powershell
cmake --build build --config Release --target Tests
'' | & .\build\src\Tests\Release\Tests.exe
```

The binary waits for Enter after completion; piping a blank line avoids an
interactive pause.

Replace `Release` with `Debug` for a Debug run. To list or select tests:

```powershell
'' | & .\build\src\Tests\Release\Tests.exe --gtest_list_tests
'' | & .\build\src\Tests\Release\Tests.exe --gtest_filter="ThreadList*"
```

## AddressSanitizer

Install Visual Studio's **C++ AddressSanitizer** component, then use a separate
build directory:

```powershell
.\scripts\generate-vs.ps1 -BuildDir build-asan -Asan
cmake --build build-asan --config Debug --target Tests Runner
'' | & .\build-asan\src\Tests\Debug\Tests.exe
```

CMake copies the ASan runtime beside `Tests.exe` and `Runner.exe`.
[CI](../../.github/workflows/test.yml) runs Debug/Release unit tests plus an
ASan Debug build. It also runs the [RUM integration test](../integration-tests/README.md)
under ASan.
