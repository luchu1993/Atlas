# atlas_clrscript — .NET CoreCLR embedding and interop

atlas_add_library(atlas_clrscript
    SOURCES
        base_native_provider.cc
        clr_bootstrap.cc
        clr_error.cc
        clr_host.cc
        $<$<PLATFORM_ID:Windows>:clr_host_windows.cc>
        $<$<PLATFORM_ID:Linux>:clr_host_linux.cc>
        clr_hot_reload.cc
        clr_object.cc
        clr_object_registry.cc
        clr_script_engine.cc
        file_watcher.cc
        native_api_provider.cc
        clr_marshal.cc
    DEPS
        atlas_foundation
        atlas_platform
        atlas_math
        atlas_script
        atlas_entitydef
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

