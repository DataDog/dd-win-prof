add_executable(ProfilerInjector
    ProfilerInjector.cpp
)

# Link-time code generation (whole-program optimisation) in Release builds only.
set_property(TARGET ProfilerInjector PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
