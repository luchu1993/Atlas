# atlas_clrscript — .NET CoreCLR embedding and interop

atlas_add_library(atlas_clrscript
    SOURCES
        clr_host.cpp
        $<$<PLATFORM_ID:Windows>:clr_host_windows.cpp>
        $<$<PLATFORM_ID:Linux>:clr_host_linux.cpp>
        native_api_provider.cpp
    DEPS
        atlas_foundation
        atlas_platform
        atlas_script
)

# Link against nethost (provides get_hostfxr_path).
# hostfxr itself is loaded at runtime via DynamicLibrary — do NOT link it here.
target_link_libraries(atlas_clrscript
    PRIVATE ${DOTNET_NETHOST_LIB}
)

# nethost.h / hostfxr.h / coreclr_delegates.h (located by FindDotNet.cmake)
target_include_directories(atlas_clrscript
    PRIVATE ${DOTNET_HOSTFXR_INCLUDE}
)

