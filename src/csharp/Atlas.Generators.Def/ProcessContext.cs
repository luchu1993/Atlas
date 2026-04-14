namespace Atlas.Generators.Def;

internal enum ProcessContext
{
    Server, // All-in-one fallback (no ATLAS_* preprocessor symbol)
    Base,
    Cell,
    Client,
}

internal enum RpcRole
{
    Send,      // Generate a send stub (serialize + call NativeApi)
    Receive,   // Generate a partial method declaration (user implements)
    Forbidden, // Generate a throw stub (client calling non-exposed method)
}

internal static class RpcRoleHelper
{
    /// <summary>
    /// Determine the RPC role for a method given its section and the current process context.
    /// See DEF_GENERATOR_DESIGN.md section 4.1 for the full matrix.
    /// </summary>
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

            // Server mode: base_methods receive, cell_methods receive, client_methods send
            ("base_methods", ProcessContext.Server)    => RpcRole.Receive,
            ("cell_methods", ProcessContext.Server)    => RpcRole.Receive,

            _ => RpcRole.Forbidden,
        };
    }
}
