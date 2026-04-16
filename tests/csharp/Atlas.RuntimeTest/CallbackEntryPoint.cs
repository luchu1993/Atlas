using System;
using System.Runtime.InteropServices;
using System.Text;
using Atlas.Core;

namespace Atlas.RuntimeTest;

// ============================================================================
// CallbackEntryPoint — [UnmanagedCallersOnly] test methods called from C++
// ============================================================================
//
// Each method exercises a specific aspect of the C# → C++ LibraryImport path.
// All methods return int: 0 = success, -1 = failure (error written via ErrorBridge).
//
// C++ test (test_clr_callback.cpp) binds these via ClrStaticMethod<int,...>
// and verifies the return value and any side effects.

public static class CallbackEntryPoint
{
    // =========================================================================
    // Test 1: ABI version round-trip
    // =========================================================================
    //
    // Calls AtlasGetAbiVersion() and writes the result to *out.
    // C++ verifies that the value matches ATLAS_NATIVE_API_ABI_VERSION.

    [UnmanagedCallersOnly]
    public static unsafe int GetAbiVersion(uint* out_version)
    {
        try
        {
            *out_version = NativeApi.GetAbiVersion();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Test 2: LogMessage round-trip
    // =========================================================================
    //
    // Calls AtlasLogMessage with a known UTF-8 string.
    // Verifies that the call does not crash and returns no error.
    // (C++ verifies the log sink received the message in a later test.)

    [UnmanagedCallersOnly]
    public static unsafe int LogTestMessage(byte* utf8Msg, int msgLen)
    {
        try
        {
            var span = new ReadOnlySpan<byte>(utf8Msg, msgLen);
            NativeApi.LogMessage(2 /* Info */, span);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Test 3: ServerTime round-trip
    // =========================================================================
    //
    // Calls AtlasServerTime() and writes the result to *out.
    // C++ verifies the value is >= 0 (stub returns 0.0 in Phase 2).

    [UnmanagedCallersOnly]
    public static unsafe int GetServerTime(double* out_time)
    {
        try
        {
            *out_time = NativeApi.ServerTime();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Test 4: Error bridge round-trip
    // =========================================================================
    //
    // Throws a known exception so C++ can verify it was relayed correctly via
    // ErrorBridge → clr_error_set → t_clr_error (TLS buffer).

    [UnmanagedCallersOnly]
    public static int ThrowException()
    {
        try
        {
            throw new InvalidOperationException("test-exception-message");
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Test 5: GCHandle lifecycle
    // =========================================================================
    //
    // Allocates a GCHandle for a managed string object, writes its IntPtr value
    // to *out_handle, and returns 0.  C++ creates a ClrObject from the handle
    // and verifies as_string() returns the expected value.

    [UnmanagedCallersOnly]
    public static unsafe int AllocStringHandle(IntPtr* out_handle)
    {
        try
        {
            // The managed object the C++ ClrObject will reference.
            var str = "hello from managed";
            *out_handle = GCHandleHelper.Alloc(str);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Test 6: ProcessPrefix round-trip
    // =========================================================================

    [UnmanagedCallersOnly]
    public static unsafe int GetProcessPrefix(byte* out_prefix)
    {
        try
        {
            *out_prefix = NativeApi.GetProcessPrefix();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // =========================================================================
    // Bootstrap forwarder
    // =========================================================================
    //
    // C++ must call Bootstrap.Initialize() through THIS assembly (Atlas.RuntimeTest)
    // rather than loading Atlas.Runtime.dll independently via get_method().
    // Otherwise the CLR may create two separate Assembly instances — one loaded
    // directly and one loaded as a dependency — each with its own static state,
    // causing ErrorBridge.s_setError to be null in the dependency copy.

    [UnmanagedCallersOnly]
    public static unsafe int RunBootstrap(IntPtr argsPtr, IntPtr vtableOutPtr)
    {
        try
        {
            var args = (BootstrapArgs*)argsPtr;
            var vtableOut = (ObjectVTableOut*)vtableOutPtr;

            if (args == null || vtableOut == null)
                return -1;

            // Same logic as Bootstrap.Initialize — duplicated here because
            // [UnmanagedCallersOnly] methods cannot be called from C#.
            ErrorBridge.RegisterNativeFunctions(
                args->ErrorSet,
                args->ErrorClear,
                args->ErrorGetCode);

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
            Console.Error.WriteLine($"[RunBootstrap] failed: {ex}");
            return -1;
        }
    }
}
