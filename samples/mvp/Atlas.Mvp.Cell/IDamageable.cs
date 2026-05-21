namespace Atlas.Mvp.Cell;

internal interface IDamageable
{
    int Hp { get; }
    void TakeDamage(int amount, uint attackerId);
}
