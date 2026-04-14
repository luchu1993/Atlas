using System.Collections.Generic;

namespace Atlas.Generators.Rpc;

internal sealed class RpcEntityModel
{
    public string Namespace { get; set; } = "";
    public string ClassName { get; set; } = "";
    public string TypeName { get; set; } = "";
    public List<RpcMethodModel> ClientRpcs { get; } = new();
    public List<RpcMethodModel> CellRpcs { get; } = new();
    public List<RpcMethodModel> BaseRpcs { get; } = new();
}

internal sealed class RpcMethodModel
{
    public string MethodName { get; set; } = "";
    public bool IsPartial { get; set; }
    public bool IsVoid { get; set; }
    public bool Reliable { get; set; } = true;
    public List<RpcParamModel> Parameters { get; } = new();
}

internal sealed class RpcParamModel
{
    public string ParamName { get; set; } = "";
    public string TypeFullName { get; set; } = "";
    public string WriterMethod { get; set; } = "";
    public string ReaderMethod { get; set; } = "";
    public bool IsSupported { get; set; }
    public byte DataTypeByte { get; set; }
}
