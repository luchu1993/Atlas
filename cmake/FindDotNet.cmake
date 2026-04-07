# FindDotNet.cmake
# Locate the .NET SDK, hostfxr library, and nethost headers.
#
# Sets:
#   DOTNET_FOUND            - TRUE if .NET SDK was found
#   DOTNET_EXECUTABLE       - Path to 'dotnet' command
#   DOTNET_SDK_DIR          - Root directory of the .NET installation
#   DOTNET_HOSTFXR_LIB     - Path to hostfxr shared library
#   DOTNET_HOSTFXR_INCLUDE  - Path to nethost/hostfxr/coreclr_delegates headers
#
# Provides:
#   atlas_build_csharp_project(project_dir) - CMake target that calls 'dotnet build'

# ── Locate the 'dotnet' executable ───────────────────────────────────────────

find_program(DOTNET_EXECUTABLE dotnet
    HINTS
        "$ENV{DOTNET_ROOT}"
        "$ENV{ProgramFiles}/dotnet"
        "$ENV{ProgramFiles\(x86\)}/dotnet"
        "/usr/share/dotnet"
        "/usr/local/share/dotnet"
        "/opt/dotnet"
)

if(NOT DOTNET_EXECUTABLE)
    message(FATAL_ERROR
        "Could not find 'dotnet' executable. "
        "Install the .NET SDK or set the DOTNET_ROOT environment variable.")
endif()

execute_process(
    COMMAND ${DOTNET_EXECUTABLE} --version
    OUTPUT_VARIABLE DOTNET_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _dotnet_version_rc
)

if(NOT _dotnet_version_rc EQUAL 0)
    message(FATAL_ERROR "Found dotnet at ${DOTNET_EXECUTABLE} but 'dotnet --version' failed.")
endif()

message(STATUS "Found .NET SDK ${DOTNET_VERSION} at ${DOTNET_EXECUTABLE}")

# ── Derive SDK root from dotnet executable location ───────────────────────────

# On Windows: C:/Program Files/dotnet/
# On Linux:   /usr/share/dotnet/ or ~/.dotnet/
get_filename_component(DOTNET_SDK_DIR "${DOTNET_EXECUTABLE}" DIRECTORY)

# ── Locate hostfxr shared library ─────────────────────────────────────────────

if(WIN32)
    set(_hostfxr_name "hostfxr.dll")
elseif(APPLE)
    set(_hostfxr_name "libhostfxr.dylib")
else()
    set(_hostfxr_name "libhostfxr.so")
endif()

# hostfxr lives in: <sdk_root>/host/fxr/<version>/
file(GLOB _hostfxr_version_dirs "${DOTNET_SDK_DIR}/host/fxr/*")

if(NOT _hostfxr_version_dirs)
    message(FATAL_ERROR
        "Could not find hostfxr version directory under ${DOTNET_SDK_DIR}/host/fxr/. "
        "Is a .NET runtime installed?")
endif()

list(SORT _hostfxr_version_dirs ORDER DESCENDING)
list(GET _hostfxr_version_dirs 0 _hostfxr_dir)

# On Windows find_library only searches for .lib/.a, not .dll.
# Use find_file for the actual shared library in all cases.
find_file(DOTNET_HOSTFXR_LIB
    NAMES ${_hostfxr_name}
    PATHS "${_hostfxr_dir}"
    NO_DEFAULT_PATH
)

if(NOT DOTNET_HOSTFXR_LIB)
    message(FATAL_ERROR
        "Could not find ${_hostfxr_name} in ${_hostfxr_dir}.")
endif()

message(STATUS "Found hostfxr: ${DOTNET_HOSTFXR_LIB}")

# ── Locate nethost headers ────────────────────────────────────────────────────
# The .NET SDK ships nethost.h, hostfxr.h, and coreclr_delegates.h in:
#   <sdk_root>/packs/Microsoft.NETCore.App.Host.<rid>/<version>/runtimes/<rid>/native/

if(WIN32)
    set(_host_rid "win-x64")
elseif(APPLE)
    set(_host_rid "osx-x64")
else()
    set(_host_rid "linux-x64")
endif()

file(GLOB _nethost_include_dirs
    "${DOTNET_SDK_DIR}/packs/Microsoft.NETCore.App.Host.${_host_rid}/*/runtimes/${_host_rid}/native"
)

if(NOT _nethost_include_dirs)
    # Fallback: search without RID constraint
    file(GLOB _nethost_include_dirs
        "${DOTNET_SDK_DIR}/packs/Microsoft.NETCore.App.Host.*/*/runtimes/*/native"
    )
endif()

if(_nethost_include_dirs)
    list(SORT _nethost_include_dirs ORDER DESCENDING)
    list(GET _nethost_include_dirs 0 DOTNET_HOSTFXR_INCLUDE)
    message(STATUS "Found nethost headers: ${DOTNET_HOSTFXR_INCLUDE}")
else()
    message(FATAL_ERROR
        "Could not find nethost headers (nethost.h, hostfxr.h, coreclr_delegates.h). "
        "Ensure the Microsoft.NETCore.App.Host.* pack is installed "
        "(run: dotnet workload restore).")
endif()

# ── Locate nethost static library (provides get_hostfxr_path) ────────────────
# On Windows: nethost.lib  On Linux/macOS: libnethost.a or libnethost.so

if(WIN32)
    find_file(DOTNET_NETHOST_LIB
        NAMES nethost.lib
        PATHS "${DOTNET_HOSTFXR_INCLUDE}"
        NO_DEFAULT_PATH
    )
else()
    find_library(DOTNET_NETHOST_LIB
        NAMES nethost
        PATHS "${DOTNET_HOSTFXR_INCLUDE}"
        NO_DEFAULT_PATH
    )
endif()

if(NOT DOTNET_NETHOST_LIB)
    message(FATAL_ERROR
        "Could not find nethost library (nethost.lib / libnethost.a) in "
        "${DOTNET_HOSTFXR_INCLUDE}.")
endif()

message(STATUS "Found nethost lib: ${DOTNET_NETHOST_LIB}")

set(DOTNET_FOUND TRUE)

# ── Helper: build a C# project via dotnet CLI ─────────────────────────────────
#
# Usage:
#   atlas_build_csharp_project(<relative_project_dir>)
#
# Creates a CMake custom target named csharp_<sanitized_dir> that runs
# 'dotnet build' each time CMake builds the project. Output goes to
# ${CMAKE_BINARY_DIR}/csharp/<project_dir>/.
#
# The output directory is stored in CSHARP_<UPPER_DIR>_OUTPUT_DIR.

function(atlas_build_csharp_project project_dir)
    # Sanitize the project_dir for use as a target name (replace / and . with _)
    string(REPLACE "/" "_" _target_suffix "${project_dir}")
    string(REPLACE "." "_" _target_suffix "${_target_suffix}")
    set(_target_name "csharp_${_target_suffix}")

    set(_output_dir "${CMAKE_BINARY_DIR}/csharp/${project_dir}")

    add_custom_target(${_target_name} ALL
        COMMAND ${DOTNET_EXECUTABLE} build
            "${CMAKE_SOURCE_DIR}/${project_dir}"
            --configuration $<IF:$<CONFIG:Debug>,Debug,Release>
            --output "${_output_dir}"
            --nologo
        COMMENT "Building C# project: ${project_dir}"
        VERBATIM
    )

    # Export output directory to caller scope
    string(TOUPPER "${_target_suffix}" _upper)
    set(CSHARP_${_upper}_OUTPUT_DIR "${_output_dir}" PARENT_SCOPE)
endfunction()
