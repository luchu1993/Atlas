namespace Atlas.Coro;

public enum AtlasTaskStatus : byte
{
    Pending   = 0,
    Succeeded = 1,
    Faulted   = 2,
    Canceled  = 3,
}
