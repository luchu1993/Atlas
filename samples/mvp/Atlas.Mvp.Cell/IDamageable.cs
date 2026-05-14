namespace Atlas.Mvp.Cell;

internal interface IDamageable
{
    int Hp { get; set; }
    void BroadcastDamage(int amount, uint attackerId);
}
