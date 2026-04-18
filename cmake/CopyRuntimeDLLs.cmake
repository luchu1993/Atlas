# Helper script invoked from add_custom_command to copy runtime DLLs next
# to a target exe. Gracefully handles the empty-list case that would
# otherwise break `cmake -E copy_if_different` on Windows.
#
# Expected variables:
#   TARGET_DLLS — semicolon-separated list of DLL paths (may be empty)
#   TARGET_DIR  — destination directory

if(NOT DEFINED TARGET_DIR)
  message(FATAL_ERROR "CopyRuntimeDLLs.cmake: TARGET_DIR is required")
endif()

foreach(dll IN LISTS TARGET_DLLS)
  if(dll AND EXISTS "${dll}")
    file(COPY "${dll}" DESTINATION "${TARGET_DIR}")
  endif()
endforeach()
