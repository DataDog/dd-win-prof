# Datadog Profiler Integration with Vulkan Examples

This guide explains how to integrate your `dd-win-prof.dll` profiling library with the Sascha Willems Vulkan examples.

## 🎯 **Integration Approach: Local CMake Patching**

We use **local CMake patching** rather than forking the entire Vulkan repository. This approach:
- ✅ Keeps the original repo intact
- ✅ Easy to update to newer Vulkan examples
- ✅ Minimal maintenance overhead
- ✅ Automated via scripts

## 🚀 **Quick Start**

### **1. Build Vulkan Examples with Profiling**
```powershell
# Build with profiler integration enabled
.\vulcan_quickstart.ps1 -EnableProfiler
```

### **2. Run with Profiling**
After building with profiler integration, you have multiple options:

**Option A: Use the generated scripts (Recommended)**
```cmd
# Navigate to the build output directory
cd vulkan_examples\Vulkan\build\bin\Release

# Run any example with profiler auto-start and debug logging
# Using batch files:
run-triangle-with-profiler.bat
run-gears-with-profiler.bat
run-computeheadless-with-profiler.bat

# Or using PowerShell (alternative):
.\run-triangle-with-profiler.ps1
.\run-gears-with-profiler.ps1  
.\run-computeheadless-with-profiler.ps1
```

**Option B: Set environment variables manually**
```cmd
set DD_PROFILING_ENABLED=1
set DD_TRACE_DEBUG=1
set DD_SERVICE=my-vulkan-app
triangle.exe
```

**Option C: Run from Visual Studio**
When debugging from Visual Studio, the environment variables are automatically set.

### **3. Build Without Profiling (Default)**
```powershell
# Normal build without profiler
.\vulcan_quickstart.ps1
```

### **4. Manual Profiler Patching**
```powershell
# Apply profiler patches manually
.\patch_vulkan_for_profiling.ps1

# Remove profiler patches
.\patch_vulkan_for_profiling.ps1 -Undo
```

## 🔧 **How It Works**

### **CMake Integration**
The patching script modifies `vulkan_examples/Vulkan/examples/CMakeLists.txt` to:

1. **Add profiler configuration options:**
   ```cmake
   option(ENABLE_DD_PROFILER "Enable Datadog profiler integration" OFF)
   ```

2. **Set up profiler paths:**
   ```cmake
   set(DD_PROFILER_ROOT "${CMAKE_SOURCE_DIR}/../../../src/dd-win-prof")
   set(DD_PROFILER_LIB "${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.lib")
   set(DD_PROFILER_DLL "${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.dll")
   ```

3. **Link profiler to each example:**
   ```cmake
   if(ENABLE_DD_PROFILER AND EXISTS ${DD_PROFILER_LIB})
       target_link_libraries(${EXAMPLE_NAME} ${DD_PROFILER_LIB})
       target_include_directories(${EXAMPLE_NAME} PRIVATE ${DD_PROFILER_INCLUDE_DIR})
       target_compile_definitions(${EXAMPLE_NAME} PRIVATE DD_PROFILER_ENABLED=1)
   endif()
   ```

4. **Set up environment variables:**
   ```cmake
   # For Visual Studio debugging
   set_target_properties(${EXAMPLE_NAME} PROPERTIES
       VS_DEBUGGER_ENVIRONMENT "DD_PROFILING_ENABLED=1;DD_TRACE_DEBUG=1;DD_SERVICE=vulkan-${EXAMPLE_NAME}")
   
   # Create batch scripts for command-line execution
   add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
       COMMAND ${CMAKE_COMMAND} -E echo "set DD_PROFILING_ENABLED=1" >> run-${EXAMPLE_NAME}-with-profiler.bat
       # ... additional environment setup)
   ```

5. **Copy DLL to output directory:**
   ```cmake
   add_custom_command(TARGET ${EXAMPLE_NAME} POST_BUILD
       COMMAND ${CMAKE_COMMAND} -E copy_if_different
       ${DD_PROFILER_DLL}
       $<TARGET_FILE_DIR:${EXAMPLE_NAME}>)
   ```

### **Code Integration**
In your Vulkan example code, you can now use:

```cpp
#ifdef DD_PROFILER_ENABLED
    // Your profiler initialization code
    InitializeDatadogProfiler();
#endif

// In your render loop or critical sections:
#ifdef DD_PROFILER_ENABLED
    // Profile specific operations
    ProfilerBeginSection("VulkanRender");
#endif

    // Your Vulkan rendering code here

#ifdef DD_PROFILER_ENABLED
    ProfilerEndSection("VulkanRender");
#endif
```

## 📁 **File Structure**

After integration, your structure looks like:
```
dd-win-prof/
├── src/dd-win-prof/
│   └── x64/Release/
│       ├── dd-win-prof.lib  ← Linked to examples
│       └── dd-win-prof.dll  ← Copied to bin directory
└── tests/
    ├── vulcan_quickstart.ps1           ← Main build script
    ├── patch_vulkan_for_profiling.ps1  ← CMake patcher
    └── vulkan_examples/
        └── Vulkan/
            └── examples/
                ├── CMakeLists.txt      ← Gets patched
                └── bin/Release/
                    ├── triangle.exe
                    ├── computeheadless.exe
                    └── dd-win-prof.dll ← Copied here
```

## 🔄 **Workflow Examples**

### **Development Workflow**
```powershell
# 1. First time setup with profiling
.\vulcan_quickstart.ps1 -EnableProfiler

# 2. Rebuild after profiler changes
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure

# 3. Test without profiler
.\vulcan_quickstart.ps1 -Clean
.\vulcan_quickstart.ps1
```

### **CI/Automation Workflow**
```powershell
# CI build with profiling enabled
.\vulcan_quickstart.ps1 -CI -EnableProfiler -Verbose
```

### **Update Vulkan Examples**
```powershell
# Remove patches, update, re-apply
.\patch_vulkan_for_profiling.ps1 -Undo
cd vulkan_examples\Vulkan
git pull
cd ..\..
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
```

## 🛠️ **Customization**

### **Change Profiler Paths**
Edit `patch_vulkan_for_profiling.ps1` and modify:
```cmake
set(DD_PROFILER_LIB "${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.lib")
set(DD_PROFILER_DLL "${CMAKE_SOURCE_DIR}/../../../src/x64/Release/dd-win-prof.dll")
```

### **Add More Examples**
The patching automatically applies to all examples in the `EXAMPLES` list in the CMakeLists.txt.

### **Debug vs Release**
The current setup uses Release builds. To support Debug:
```cmake
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DD_PROFILER_LIB "${CMAKE_SOURCE_DIR}/../../../src/x64/Debug/dd-win-prof.lib")
    set(DD_PROFILER_DLL "${CMAKE_SOURCE_DIR}/../../../src/x64/Debug/dd-win-prof.dll")
endif()
```

## 🔍 **Troubleshooting**

### **Profiler DLL Not Found**
```
WARN: Datadog profiler DLL not found
```
**Solution:** Build your profiler first:
```cmd
msbuild src\dd-win-prof\dd-win-prof.vcxproj /p:Configuration=Release /p:Platform=x64
```

### **Script Files Not Generated**
If you don't see the `run-*-with-profiler.bat` files in your build output:

**Check 1: Verify profiler integration was applied**
```powershell
# Look for this in your build output
[INFO] Vulkan examples patched for profiler integration
```

**Check 2: Manual verification**
```cmd
# Navigate to build directory
cd vulkan_examples\Vulkan\build\bin\Release
dir run-*-with-profiler.*

# Should show files like:
# run-triangle-with-profiler.bat
# run-triangle-with-profiler.ps1
```

**Solution:** Force rebuild with profiler integration:
```powershell
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
```

### **Patching Failed**
```
ERROR: Failed to patch Vulkan examples
```
**Solution:** Remove existing patches and retry:
```powershell
.\patch_vulkan_for_profiling.ps1 -Undo
.\patch_vulkan_for_profiling.ps1
```

### **CMake Configuration Error**
```
ERROR: CMake configuration failed
```
**Solution:** Force reconfiguration:
```powershell
.\vulcan_quickstart.ps1 -EnableProfiler -ForceReconfigure
```

## 🎉 **Success Indicators**

When profiler integration is working correctly, you'll see:

### **Build-time Success:**
```
[INFO] Vulkan examples patched for profiler integration
[OK] Added profiler flag to CMake configuration
[INFO] Datadog profiler integration enabled
[INFO] Profiler lib: C:\repos\dd-win-prof\src\x64\Release\dd-win-prof.lib
[INFO] Profiler DLL: C:\repos\dd-win-prof\src\x64\Release\dd-win-prof.dll
```

### **Runtime Success:**
After building, you should find in your `bin\Release\` directory:
- `triangle.exe`, `gears.exe`, `computeheadless.exe` (original executables)
- `dd-win-prof.dll` (profiler library)
- `run-triangle-with-profiler.bat` (batch convenience scripts)
- `run-triangle-with-profiler.ps1` (PowerShell convenience scripts)
- `run-gears-with-profiler.bat` / `run-gears-with-profiler.ps1`
- `run-computeheadless-with-profiler.bat` / `run-computeheadless-with-profiler.ps1`

When you run the batch scripts, you should see profiler debug output:
```
Starting triangle with Datadog profiler (zero-code injection)...
Datadog Profiler Injector
=========================
Successfully injected profiler DLL into process 12345
Process launched with profiler successfully
```

Your Vulkan examples will now be linked with your profiling library and ready for performance analysis! 🚀
