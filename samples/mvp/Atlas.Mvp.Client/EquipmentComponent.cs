using System;

namespace Atlas.Components;

public sealed partial class EquipmentComponent
{
    public static event Action<uint, int>? WeaponChanged;

    partial void OnWeaponIdChanged(int oldValue, int newValue) =>
        WeaponChanged?.Invoke(Entity.EntityId, newValue);
}
