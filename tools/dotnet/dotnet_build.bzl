# tools/dotnet/dotnet_build.bzl
#
# Starlark macro for building C# projects via `dotnet build`.
# Wraps `dotnet build` in a genrule so Bazel tracks inputs/outputs
# and skips rebuilds when sources haven't changed.

def atlas_dotnet_project(
        name,
        project_file,
        assembly_name,
        srcs = None,
        project_deps = None,
        extra_outputs = None,
        configuration = "Release",
        needs_restore = False,
        visibility = None,
        tags = None):
    """Build a C# project via dotnet CLI.

    Args:
        name: Bazel target name.
        project_file: Label for the .csproj file (e.g. "Atlas.SmokeTest.csproj").
        assembly_name: Output assembly name (e.g. "Atlas.SmokeTest.dll").
        srcs: Source files to track for rebuild.
        project_deps: Labels of other atlas_dotnet_project targets this depends on.
        extra_outputs: Additional output files to declare (e.g. .pdb, deps.json).
        configuration: Build configuration (Debug or Release).
        needs_restore: Set True for projects with NuGet package references.
        visibility: Bazel visibility.
        tags: Bazel tags.
    """
    if srcs == None:
        srcs = native.glob(["**/*.cs", "*.csproj"])

    deps = []
    if project_deps:
        deps = project_deps

    all_outs = [assembly_name]
    if extra_outputs:
        all_outs = all_outs + extra_outputs

    restore_flag = "" if needs_restore else " --no-restore"

    # On Windows, Bazel's execroot uses directory junctions which confuse
    # NuGet path resolution. We resolve the real workspace root via the
    # MODULE.bazel junction and build from there.
    #
    # For projects without NuGet dependencies, we skip restore (--no-restore)
    # to avoid NuGet path issues entirely. Projects with NuGet deps should
    # run `dotnet restore` from the workspace root before building with Bazel.
    native.genrule(
        name = name,
        srcs = srcs + deps,
        outs = all_outs,
        cmd = select({
            "@platforms//os:windows": " && ".join([
                "WS_ROOT=$$(dirname $$(readlink -f MODULE.bazel))",
                "REAL_PROJ=$$(readlink -f $(rootpath {proj}))".format(proj = project_file),
                "ABS_OUTDIR=$$(cygpath -wa $(@D))",
                "cd $$WS_ROOT",
                "dotnet build \"$$(cygpath -w $$REAL_PROJ)\" --configuration {cfg} --output \"$$ABS_OUTDIR\" --nologo -v quiet{restore}".format(
                    cfg = configuration,
                    restore = restore_flag,
                ),
            ]),
            "//conditions:default": " && ".join([
                "WS_ROOT=$$(dirname $$(readlink -f MODULE.bazel))",
                "REAL_PROJ=$$(readlink -f $(rootpath {proj}))".format(proj = project_file),
                "ABS_OUTDIR=$$(cd $(@D) && pwd)",
                "cd $$WS_ROOT",
                "dotnet build \"$$REAL_PROJ\" --configuration {cfg} --output \"$$ABS_OUTDIR\" --nologo -v quiet{restore}".format(
                    cfg = configuration,
                    restore = restore_flag,
                ),
            ]),
        }),
        local = True,
        visibility = visibility,
        tags = tags,
    )
