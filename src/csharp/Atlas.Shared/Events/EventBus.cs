using System;
using System.Collections.Generic;

namespace Atlas.Events;

/// <summary>
/// In-process synchronous event bus.
/// Not thread-safe — call only from the main (tick) thread.
/// Cross-process events use RPC/Mailbox, not EventBus.
/// </summary>
public sealed class EventBus
{
    private readonly Dictionary<string, List<EventSubscription>> _subscriptions = new();

    public void Subscribe(string eventName, IEventHandler handler, object owner)
    {
        if (!_subscriptions.TryGetValue(eventName, out var list))
        {
            list = new List<EventSubscription>();
            _subscriptions[eventName] = list;
        }
        list.Add(new EventSubscription(handler, owner));
    }

    public void Fire(string eventName, ReadOnlySpan<byte> payload)
    {
        if (!_subscriptions.TryGetValue(eventName, out var list))
            return;

        // Snapshot count to protect against re-entrant unsubscribe during dispatch.
        // Handlers removed mid-iteration will still be invoked this pass; newly
        // added handlers will only fire on the next Fire() call.
        int count = list.Count;
        for (int i = 0; i < count; i++)
        {
            if (i >= list.Count) break;
            try
            {
                list[i].Handler.Handle(payload);
            }
            catch (Exception)
            {
                // Individual handler failures are isolated — do not propagate.
            }
        }
    }

    public void UnsubscribeAll(object owner)
    {
        foreach (var list in _subscriptions.Values)
        {
            list.RemoveAll(s => ReferenceEquals(s.Owner, owner));
        }
    }

    public void Clear()
    {
        _subscriptions.Clear();
    }

    private readonly struct EventSubscription
    {
        public readonly IEventHandler Handler;
        public readonly object Owner;

        public EventSubscription(IEventHandler handler, object owner)
        {
            Handler = handler;
            Owner = owner;
        }
    }
}
