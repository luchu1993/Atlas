using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Atlas.Core;

// ============================================================================
// GCHandleHelper — GCHandle lifecycle management for ClrObject (C++ side)
// ============================================================================
//
// ClrObject (C++) holds a GCHandle (void*) that keeps a managed object alive.
// The [UnmanagedCallersOnly] methods below form the vtable that C++ registers
// into ClrObjectVTable during CLR bootstrap.
//
// GCHandle allocation policy:
//   - All handles are GCHandleType.Normal (pinning is not needed; C++ stores
//     the opaque handle, not a raw pointer into managed memory).
//   - Handles are freed exactly once by ClrObject::release().
//
// Lives in Atlas.ClrHost alongside Bootstrap/ErrorBridge so atlas_server and
// atlas_client share one definition (and one GCHandle-release contract).
// Unity hosts skip this assembly entirely.

public static unsafe class GCHandleHelper
{
    // -------------------------------------------------------------------------
    // GCHandle factory — called from C# when creating a ClrObject
    // -------------------------------------------------------------------------

    /// <summary>
    /// Allocate a Normal GCHandle for <paramref name="obj"/> and return
    /// the handle as an <see cref="IntPtr"/> (= void* on C++ side).
    /// </summary>
    public static IntPtr Alloc(object obj)
    {
        return GCHandle.ToIntPtr(GCHandle.Alloc(obj, GCHandleType.Normal));
    }

    // -------------------------------------------------------------------------
    // [UnmanagedCallersOnly] vtable methods — called from C++ ClrObject
    // -------------------------------------------------------------------------

    /// <summary>Free a GCHandle previously allocated by <see cref="Alloc"/>.</summary>
    [UnmanagedCallersOnly]
    public static void FreeHandle(IntPtr handlePtr)
    {
        try
        {
            GCHandle.FromIntPtr(handlePtr).Free();
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    /// <summary>
    /// Write the CLR type name of the object into <paramref name="buf"/>.
    /// Returns the number of UTF-8 bytes written, or -1 on error.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int GetTypeName(IntPtr handlePtr, byte* buf, int bufLen)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target;
            var name = target?.GetType().Name ?? "null";
            if (buf == null || bufLen <= 0)
                return Encoding.UTF8.GetByteCount(name);
            return Encoding.UTF8.GetBytes(name, new Span<byte>(buf, bufLen));
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Returns 1 if the object is null, 0 otherwise.
    /// </summary>
    [UnmanagedCallersOnly]
    public static byte IsNone(IntPtr handlePtr)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target;
            return target is null ? (byte)1 : (byte)0;
        }
        catch
        {
            return 1;
        }
    }

    /// <summary>
    /// Converts the object to <see cref="long"/> via <see cref="Convert.ToInt64"/>.
    /// Writes the result to <paramref name="outVal"/>; returns 0 on success, -1 on error.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int ToInt64(IntPtr handlePtr, long* outVal)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target
                ?? throw new NullReferenceException("Managed object is null");
            *outVal = Convert.ToInt64(target);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Converts the object to <see cref="double"/> via <see cref="Convert.ToDouble"/>.
    /// Writes the result to <paramref name="outVal"/>; returns 0 on success, -1 on error.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int ToDouble(IntPtr handlePtr, double* outVal)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target
                ?? throw new NullReferenceException("Managed object is null");
            *outVal = Convert.ToDouble(target);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Converts the object to its string representation (UTF-8).
    /// If <paramref name="buf"/> is null or <paramref name="bufLen"/> is 0,
    /// returns the required byte count.  Otherwise writes up to
    /// <paramref name="bufLen"/> bytes and returns the number written.
    /// Returns -1 on error.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int ToString(IntPtr handlePtr, byte* buf, int bufLen)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target;
            var str = target?.ToString() ?? string.Empty;

            if (buf == null || bufLen <= 0)
                return Encoding.UTF8.GetByteCount(str);

            return Encoding.UTF8.GetBytes(str, new Span<byte>(buf, bufLen));
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    /// <summary>
    /// Converts the object to <see cref="bool"/> via <see cref="Convert.ToBoolean"/>.
    /// Writes 0 or 1 to <paramref name="outVal"/>; returns 0 on success, -1 on error.
    /// </summary>
    [UnmanagedCallersOnly]
    public static int ToBool(IntPtr handlePtr, byte* outVal)
    {
        try
        {
            var target = GCHandle.FromIntPtr(handlePtr).Target
                ?? throw new NullReferenceException("Managed object is null");
            *outVal = Convert.ToBoolean(target) ? (byte)1 : (byte)0;
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }
}
