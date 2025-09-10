# ProfilerInjector

A Windows utility for zero-code profiling by injecting the Datadog profiler DLL into target applications. This is intended for test purposes.

## Usage

```cmd
ProfilerInjector.exe <target_executable> [arguments...]
```

**Examples:**
```cmd
# Basic usage
ProfilerInjector.exe myapp.exe

# With arguments
ProfilerInjector.exe myapp.exe --config config.json --verbose
```

## Configuration

### Option 1: .env File (Recommended)

ProfilerInjector automatically searches for `.env` files in this order:

1. **Same directory as executable** (highest priority)
2. **Current working directory**  
3. **Parent directory** (useful for build directories)
4. **Two levels up** (for nested builds like `build/bin/Release/`)

Create a `.env` file in any of these locations:

```env
# .env file example
DD_SITE=datadoghq.eu
DD_SERVICE=my-service
DD_ENV=production
DD_VERSION=1.2.3
DD_TRACE_DEBUG=1
# Comments are supported
DD_API_KEY=your_api_key_here
```

**Example locations for Vulkan builds:**
```
vulkan_examples/
├── .env                    # ← Works! (2 levels up from executable)
└── build/
    └── bin/
        └── Release/
            ├── .env        # ← Works! (same directory)
            ├── triangle.exe
            └── ProfilerInjector.exe
```

Then run:
```cmd
ProfilerInjector.exe myapp.exe
```

### Option 2: Environment Variables

Set variables manually before running:

```cmd
set DD_SITE=datadoghq.eu
set DD_SERVICE=my-service
set DD_TRACE_DEBUG=1
ProfilerInjector.exe myapp.exe
```

## Environment Variables

- `DD_PROFILING_AUTO_START=1` - Automatically start profiling when DLL loads
- `DD_TRACE_DEBUG=1` - Enable debug logging  
- `DD_SERVICE=<name>` - Set service name for profiling data
- `DD_PROFILING_ENABLED=false` - Disable profiling completely

## Building

Built as part of the main solution:

```cmd
msbuild src\WindowsProfiler.sln /p:Configuration=Release /p:Platform=x64
```

Output: `src\x64\Release\ProfilerInjector.exe`
