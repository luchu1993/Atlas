namespace Atlas.Rpc;

public enum MailboxTarget : byte
{
    OwnerClient  = 0,
    AllClients   = 1,
    OtherClients = 2,
}
