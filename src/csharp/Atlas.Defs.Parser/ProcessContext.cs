namespace Atlas.Generators.Def;

internal enum ProcessContext
{
    // Fallback when none of ATLAS_BASE/CELL/CLIENT is defined — emits every
    // RPC direction. Not a synonym for "server-side"; Base and Cell are too.
    Server,
    Base,
    Cell,
    Client,
}

internal enum RpcRole
{
    Send,
    Receive,
    Forbidden,
}

internal static class RpcRoleHelper
{
    // See def_generator_design.md §3.1 for the full matrix.
    public static RpcRole GetRole(string section, ProcessContext ctx, ExposedScope exposed)
    {
        return (section, ctx) switch
        {
            ("client_methods", ProcessContext.Client) => RpcRole.Receive,
            ("client_methods", _)                     => RpcRole.Send,

            ("cell_methods", ProcessContext.Cell)      => RpcRole.Receive,
            ("cell_methods", ProcessContext.Base)      => RpcRole.Send,
            ("cell_methods", ProcessContext.Client)    => exposed != ExposedScope.None
                                                           ? RpcRole.Send : RpcRole.Forbidden,

            ("base_methods", ProcessContext.Base)      => RpcRole.Receive,
            ("base_methods", ProcessContext.Cell)      => RpcRole.Send,
            ("base_methods", ProcessContext.Client)    => exposed != ExposedScope.None
                                                           ? RpcRole.Send : RpcRole.Forbidden,

            ("base_methods", ProcessContext.Server)    => RpcRole.Receive,
            ("cell_methods", ProcessContext.Server)    => RpcRole.Receive,

            _ => RpcRole.Forbidden,
        };
    }
}
