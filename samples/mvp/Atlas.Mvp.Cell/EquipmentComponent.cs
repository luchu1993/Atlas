namespace Atlas.Components;

public sealed partial class EquipmentComponent
{
    public partial void EquipWeapon(int weaponId)
    {
        if (weaponId < 0 || weaponId > 3) return;
        WeaponId = weaponId;
    }
}
