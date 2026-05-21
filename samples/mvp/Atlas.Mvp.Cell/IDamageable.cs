namespace Atlas.Mvp.Cell;

internal interface IDamageable
{
    int Hp { get; }
    // Cross-cellapp safe: simulator on shooter's cell invokes locally when the
    // target is Real, or routes via NativeApi.SendCellRpc when it's a Ghost.
    void TakeDamage(int amount, uint attackerId);
}
