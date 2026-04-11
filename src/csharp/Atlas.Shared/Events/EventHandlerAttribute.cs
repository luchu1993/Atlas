using System;

namespace Atlas.Events;

[AttributeUsage(AttributeTargets.Method)]
public sealed class EventHandlerAttribute : Attribute
{
    public string EventName { get; }
    public EventHandlerAttribute(string eventName) => EventName = eventName;
}

/// <summary>
/// Marker interface for classes that contain [EventHandler] methods.
/// </summary>
public interface IEventListener { }

/// <summary>
/// Handler interface for EventBus. Source Generator generates implementations
/// that deserialize ReadOnlySpan&lt;byte&gt; payload and call the user method.
/// SpanReader is a ref struct and cannot be used in Action&lt;T&gt; generics,
/// so this interface receives raw bytes instead.
/// </summary>
public interface IEventHandler
{
    void Handle(ReadOnlySpan<byte> payload);
}
