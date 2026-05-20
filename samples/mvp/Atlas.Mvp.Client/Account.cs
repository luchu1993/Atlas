using System;
using Atlas.Client;

namespace Atlas.Mvp.Client;

[Atlas.Entity.Entity("Account")]
public partial class Account : ClientEntity
{
    public event Action? PersistentInfoChanged;

    partial void OnLoginCountChanged(int oldValue, int newValue) => PersistentInfoChanged?.Invoke();
    partial void OnLastLoginChanged(string oldValue, string newValue) => PersistentInfoChanged?.Invoke();
}
