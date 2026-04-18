# FindDotNet.cmake
#
# Locates the .NET SDK and creates an IMPORTED target DotNet::nethost
# for linking against the nethost/hostfxr native libraries.
#
# Sets:
#   DOTNET_FOUND             - TRUE if .NET SDK was found
#   DOTNET_EXECUTABLE        - Path to dotnet executable
#   DOTNET_ROOT              - .NET SDK root directory
#   DOTNET_RUNTIME_VERSION   - Detected runtime version (e.g. "9.0.0")
#   DOTNET_RUNTIME_TFM       - Target framework moniker (e.g. "net9.0")
#
# Creates:
#   DotNet::nethost          - IMPORTED library target

if(TARGET DotNet::nethost)
  return()
endif()

# ── Find dotnet executable ───────────────────────────────────────────────────
find_program(DOTNET_EXECUTABLE dotnet)

# ── Find DOTNET_ROOT ─────────────────────────────────────────────────────────
if(DEFINED ENV{DOTNET_ROOT} AND EXISTS "$ENV{DOTNET_ROOT}")
  set(_dotnet_root "$ENV{DOTNET_ROOT}")
elseif(DOTNET_EXECUTABLE)
  # Derive root from dotnet executable location
  cmake_path(GET DOTNET_EXECUTABLE PARENT_PATH _dotnet_root)
else()
  # Well-known defaults
  if(WIN32)
    set(_candidates "C:/Program Files/dotnet")
  else()
    set(_candidates "/usr/share/dotnet" "/usr/lib/dotnet" "/usr/local/share/dotnet")
  endif()
  foreach(_c IN LISTS _candidates)
    if(EXISTS "${_c}")
      set(_dotnet_root "${_c}")
      break()
    endif()
  endforeach()
endif()

if(NOT _dotnet_root)
  message(WARNING "Could not locate .NET SDK. Set DOTNET_ROOT env var. CLR features disabled.")
  set(DOTNET_FOUND FALSE)
  return()
endif()

set(DOTNET_ROOT "${_dotnet_root}" CACHE PATH ".NET SDK root directory")

# ── Detect runtime version ───────────────────────────────────────────────────
if(DOTNET_EXECUTABLE)
  execute_process(
    COMMAND "${DOTNET_EXECUTABLE}" --list-runtimes
    OUTPUT_VARIABLE _runtimes_output
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _runtimes_result
  )
  if(_runtimes_result EQUAL 0)
    # Find the latest Microsoft.NETCore.App version
    string(REGEX MATCHALL "Microsoft\\.NETCore\\.App ([0-9]+\\.[0-9]+\\.[0-9]+)" _matches "${_runtimes_output}")
    if(_matches)
      # Get the last match (latest version)
      list(GET _matches -1 _last_match)
      string(REGEX MATCH "([0-9]+\\.[0-9]+\\.[0-9]+)" DOTNET_RUNTIME_VERSION "${_last_match}")
      string(REGEX MATCH "([0-9]+\\.[0-9]+)" _major_minor "${DOTNET_RUNTIME_VERSION}")
      set(DOTNET_RUNTIME_TFM "net${_major_minor}")
    endif()
  endif()
endif()

if(NOT DOTNET_RUNTIME_VERSION)
  message(WARNING "Could not detect .NET runtime version.")
  set(DOTNET_FOUND FALSE)
  return()
endif()

message(STATUS "Found .NET SDK: ${DOTNET_ROOT}")
message(STATUS "  Runtime: ${DOTNET_RUNTIME_VERSION} (${DOTNET_RUNTIME_TFM})")

# ── Find nethost native pack ────────────────────────────────────────────────
if(WIN32)
  set(_rid "win-x64")
elseif(APPLE)
  if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(_rid "osx-arm64")
  else()
    set(_rid "osx-x64")
  endif()
else()
  set(_rid "linux-x64")
endif()

set(_packs_dir "${DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${_rid}")

if(NOT EXISTS "${_packs_dir}")
  message(WARNING "Cannot find .NET host pack at: ${_packs_dir}")
  set(DOTNET_FOUND FALSE)
  return()
endif()

# Find the latest version directory with nethost.h
file(GLOB _version_dirs "${_packs_dir}/*")
list(SORT _version_dirs ORDER DESCENDING)

set(_native_dir "")
foreach(_vdir IN LISTS _version_dirs)
  set(_candidate "${_vdir}/runtimes/${_rid}/native")
  if(EXISTS "${_candidate}/nethost.h")
    set(_native_dir "${_candidate}")
    break()
  endif()
endforeach()

if(NOT _native_dir)
  message(WARNING "Found .NET host pack but no version contains nethost.h")
  set(DOTNET_FOUND FALSE)
  return()
endif()

message(STATUS "  Native pack: ${_native_dir}")

# ── Create IMPORTED target ───────────────────────────────────────────────────
add_library(DotNet::nethost SHARED IMPORTED GLOBAL)
set_target_properties(DotNet::nethost PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${_native_dir}"
)

if(WIN32)
  set_target_properties(DotNet::nethost PROPERTIES
    IMPORTED_IMPLIB "${_native_dir}/nethost.lib"
    IMPORTED_LOCATION "${_native_dir}/nethost.dll"
  )
else()
  set_target_properties(DotNet::nethost PROPERTIES
    IMPORTED_LOCATION "${_native_dir}/libnethost.so"
  )
endif()

set(DOTNET_FOUND TRUE)

# ── Generate runtimeconfig.json ──────────────────────────────────────────────
configure_file(
  "${CMAKE_SOURCE_DIR}/runtime/atlas_server.runtimeconfig.json.in"
  "${CMAKE_BINARY_DIR}/runtime/atlas_server.runtimeconfig.json"
  @ONLY
)
