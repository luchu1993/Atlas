using System;
using System.Text;
using System.Runtime.InteropServices;

namespace Atlas.Core;

// ============================================================================
// ErrorBridge — C# exception → C++ error buffer relay
// ============================================================================
//
// [UnmanagedCallersOnly] methods cannot propagate managed exceptions across
// the native/managed boundary (doing so terminates the process).  Every C#
// entry point must catch all exceptions and relay them to the C++ TLS error
// buffer via this class.
//
// The C++ side provides three function pointers (registered during bootstrap):
//   clr_error_set   — write error code + UTF-8 message into the TLS buffer
//   clr_error_clear — clear the TLS buffer
//   clr_error_get_code — read the current error code
//
// Usage pattern in every [UnmanagedCallersOnly] method:
//
//   [UnmanagedCallersOnly]
//   public static int SomeMethod(int arg)
//   {
//       try
//       {
//           DoWork(arg);
//           return 0;
//       }
//       catch (Exception ex)
//       {
//           ErrorBridge.SetError(ex);
//           return -1;
//       }
//   }

internal static unsafe class ErrorBridge
{
    // Static function pointer fields — populated once during bootstrap.
    private static delegate* unmanaged<int, byte*, int, void> s_setError;
    private static delegate* unmanaged<void> s_clearError;
    private static delegate* unmanaged<int> s_getErrorCode;

    // ---- Bootstrap ----

    /// <summary>
    /// Register the C++ TLS error-buffer functions.  Must be called once
    /// during CLR initialization, before any [UnmanagedCallersOnly] entry
    /// point can be invoked.
    /// Called from Bootstrap.Initialize() (which is itself an [UnmanagedCallersOnly]
    /// entry point, but this method is NOT — it is called as normal managed code).
    /// </summary>
    public static void RegisterNativeFunctions(
        delegate* unmanaged<int, byte*, int, void> setErrorFn,
        delegate* unmanaged<void> clearErrorFn,
        delegate* unmanaged<int> getCodeFn)
    {
        s_setError = setErrorFn;
        s_clearError = clearErrorFn;
        s_getErrorCode = getCodeFn;
    }

    // ---- Public API (called from every [UnmanagedCallersOnly] catch block) ----

    /// <summary>
    /// Write the exception into the C++ TLS error buffer.
    /// Thread-safe: writes go into the calling thread's TLS slot.
    /// </summary>
    public static void SetError(Exception ex)
    {
        var fn = s_setError;
        if (fn == null)
            return;

        var message = ex.Message ?? string.Empty;
        var byteCount = Encoding.UTF8.GetByteCount(message);

        const int StackThreshold = 512;
        if (byteCount <= StackThreshold)
        {
            byte* buf = stackalloc byte[byteCount];
            int written = Encoding.UTF8.GetBytes(message, new Span<byte>(buf, byteCount));
            fn(ex.HResult, buf, written);
        }
        else
        {
            var bytes = Encoding.UTF8.GetBytes(message);
            fixed (byte* ptr = bytes)
                fn(ex.HResult, ptr, bytes.Length);
        }
    }

    /// <summary>Clear the C++ TLS error buffer for the calling thread.</summary>
    public static void Clear()
    {
        var fn = s_clearError;
        if (fn != null) fn();
    }

    /// <summary>Return the current error code from the C++ TLS buffer (0 = no error).</summary>
    public static int GetErrorCode()
    {
        var fn = s_getErrorCode;
        return fn != null ? fn() : 0;
    }
}
