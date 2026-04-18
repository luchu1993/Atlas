# AtlasRuntimeDeploy.cmake
#
# Deploys CLR runtime DLLs next to executables that need them on Windows.
#
# Two DLLs are involved:
#   - atlas_engine.dll  — shared library loaded at runtime via
#                         DynamicLibrary::Load from argv[0].parent_path()
#                         (see src/lib/server/script_app.cc). Must sit next
#                         to the exe, regardless of link-time dependencies.
#   - nethost.dll       — .NET host library, implicitly imported by
#                         atlas_clrscript at link time. Any exe that
#                         statically links atlas_clrscript (directly or
#                         transitively) requires nethost.dll at process
#                         startup or the OS loader fails with
#                         STATUS_DLL_NOT_FOUND (0xc0000135) before main().
#
# On non-Windows platforms the helper is a no-op (shared objects are
# resolved via RPATH / LD_LIBRARY_PATH).

# ── Transitive link walk ────────────────────────────────────────────────────
#
# Breadth-first search through LINK_LIBRARIES / INTERFACE_LINK_LIBRARIES
# starting at `target`. Sets `out_var` to TRUE if `dep` is reachable.
function(_atlas_transitively_links target dep out_var)
  set(_queue ${target})
  set(_seen "")
  while(_queue)
    list(POP_FRONT _queue _cur)
    if(_cur IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen ${_cur})
    if(_cur STREQUAL dep)
      set(${out_var} TRUE PARENT_SCOPE)
      return()
    endif()
    if(NOT TARGET ${_cur})
      continue()
    endif()
    get_target_property(_type ${_cur} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(_links ${_cur} INTERFACE_LINK_LIBRARIES)
    else()
      get_target_property(_links ${_cur} LINK_LIBRARIES)
      get_target_property(_iface ${_cur} INTERFACE_LINK_LIBRARIES)
      if(_iface)
        if(_links)
          list(APPEND _links ${_iface})
        else()
          set(_links ${_iface})
        endif()
      endif()
    endif()
    if(_links)
      foreach(_lib IN LISTS _links)
        # Skip generator expressions and non-target tokens
        if(NOT _lib MATCHES "^\\$<" AND NOT _lib IN_LIST _seen)
          list(APPEND _queue ${_lib})
        endif()
      endforeach()
    endif()
  endwhile()
  set(${out_var} FALSE PARENT_SCOPE)
endfunction()

# ── Public helper ───────────────────────────────────────────────────────────
#
# Copies atlas_engine.dll and nethost.dll next to `target`'s output file at
# POST_BUILD. Idempotent — calling twice on the same target is safe.
function(atlas_deploy_clr_runtime target)
  if(NOT WIN32)
    return()
  endif()
  if(NOT TARGET atlas_engine)
    message(FATAL_ERROR
      "atlas_deploy_clr_runtime(${target}): atlas_engine target not defined "
      "(CLR features disabled?)")
  endif()

  get_target_property(_already_deployed ${target} _ATLAS_CLR_DEPLOYED)
  if(_already_deployed)
    return()
  endif()
  set_target_properties(${target} PROPERTIES _ATLAS_CLR_DEPLOYED TRUE)

  # Ensure atlas_engine is built before `target` finishes linking, so the
  # POST_BUILD copy finds the DLL in place.
  add_dependencies(${target} atlas_engine)

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:atlas_engine>"
            "$<TARGET_FILE_DIR:${target}>"
    VERBATIM
  )

  if(TARGET DotNet::nethost)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:DotNet::nethost>"
              "$<TARGET_FILE_DIR:${target}>"
      VERBATIM
    )
  endif()
endfunction()

# Calls atlas_deploy_clr_runtime(${target}) only if the target transitively
# links atlas_clrscript. For use in test helpers where the CLR dependency
# may or may not be present depending on DEPS.
function(atlas_deploy_clr_runtime_if_needed target)
  if(NOT WIN32)
    return()
  endif()
  _atlas_transitively_links(${target} atlas_clrscript _need_clr)
  if(_need_clr)
    atlas_deploy_clr_runtime(${target})
  endif()
endfunction()
