namespace Atlas.Coro;

// Void marker for the non-generic AtlasTask. Lets a single AtlasTaskSourceBox<,>
// implementation back both AtlasTask and AtlasTask<T>.
public readonly struct AtlasUnit
{
    public static readonly AtlasUnit Default = default;
}
