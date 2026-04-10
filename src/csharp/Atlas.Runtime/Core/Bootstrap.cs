using System;
using System.Runtime.InteropServices;

namespace Atlas.Core;

// ============================================================================
// Bootstrap — CLR initialization entry point called from ClrHost (C++)
// ============================================================================
//
// C++ calls Bootstrap.Initialize() once after loading the CLR.
// This method:
//   1. Registers the C++ error-bridge function pointers with ErrorBridge.
//   2. Populates ClrObjectVTable by returning function pointers to C++.
//
// The C++ side uses ClrHost::get_method_as<> to bind this method:
//   using InitFn = int(*)(BootstrapArgs*);
//   auto fn = host.get_method_as<InitFn>(asm, "Atlas.Core.Bootstrap, Atlas.Runtime", "Initialize");
//
// BootstrapArgs is defined in clr_bootstrap_args.hpp (C++) and must match
// the BootstrapArgs struct below byte-for-byte.

/// <summary>
/// Function-pointer bundle passed from C++ to Bootstrap.Initialize().
/// All pointers must be non-null.
/// Layout must match <c>ClrBootstrapArgs</c> in C++.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct BootstrapArgs
{
    // ---- Error bridge (C++ TLS functions) ----
    public delegate* unmanaged<int, byte*, int, void> ErrorSet;
    public delegate* unmanaged<void> ErrorClear;
    public delegate* unmanaged<int> ErrorGetCode;
}

/// <summary>
/// Function-pointer bundle written by Bootstrap.Initialize() and read by C++
/// to populate ClrObjectVTable.
/// Layout must match <c>ClrObjectVTableOut</c> in C++.
/// </summary>
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct ObjectVTableOut
{
    public delegate* unmanaged<IntPtr, void> FreeHandle;
    public delegate* unmanaged<IntPtr, byte*, int, int> GetTypeName;
    public delegate* unmanaged<IntPtr, byte> IsNone;
    public delegate* unmanaged<IntPtr, long*, int> ToInt64;
    public delegate* unmanaged<IntPtr, double*, int> ToDouble;
    public delegate* unmanaged<IntPtr, byte*, int, int> ToStr;   // avoids shadow of object.ToString()
    public delegate* unmanaged<IntPtr, byte*, int> ToBool;
}

internal static unsafe class Bootstrap
{
    /// <summary>
    /// Entry point called by C++ immediately after loading the CLR.
    /// </summary>
    /// <param name="args">Pointer to a <see cref="BootstrapArgs"/> struct.</param>
    /// <param name="vtableOut">Pointer to a <see cref="ObjectVTableOut"/> struct to fill.</param>
    /// <returns>0 on success, -1 on failure (error written to C++ stderr).</returns>
    [UnmanagedCallersOnly]
    public static int Initialize(BootstrapArgs* args, ObjectVTableOut* vtableOut)
    {
        try
        {
            if (args == null || vtableOut == null)
                throw new ArgumentNullException("args or vtableOut is null");

            // 1. Register error bridge so subsequent C# methods can report errors.
            ErrorBridge.RegisterNativeFunctions(
                args->ErrorSet,
                args->ErrorClear,
                args->ErrorGetCode);

            // 2. Fill vtable with GCHandleHelper function pointers.
            vtableOut->FreeHandle = &GCHandleHelper.FreeHandle;
            vtableOut->GetTypeName = &GCHandleHelper.GetTypeName;
            vtableOut->IsNone = &GCHandleHelper.IsNone;
            vtableOut->ToInt64 = &GCHandleHelper.ToInt64;
            vtableOut->ToDouble = &GCHandleHelper.ToDouble;
            vtableOut->ToStr = &GCHandleHelper.ToString;
            vtableOut->ToBool = &GCHandleHelper.ToBool;

            return 0;
        }
        catch (Exception ex)
        {
            // ErrorBridge may not yet be registered; fall back to stderr.
            Console.Error.WriteLine($"[Atlas.Bootstrap] Initialize failed: {ex}");
            return -1;
        }
    }
}
