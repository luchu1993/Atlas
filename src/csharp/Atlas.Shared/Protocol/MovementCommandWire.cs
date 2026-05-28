namespace Atlas.Shared.Protocol;

public enum MovementCommandType : byte
{
    Dash = 0,
    Launch = 1,
    LaunchOther = 2,
    Pull = 3,
    Knockback = 4,
    Teleport = 5,
    FollowEntity = 6,
}

public enum MovementCommandInputPolicy : byte
{
    Suppress = 0,
    AllowTurn = 1,
    AllowFull = 2,
}

public enum MovementCommandCollisionPolicy : byte
{
    Stop = 0,
    Continue = 1,
    EndSkill = 2,
}

public enum MovementCommandEndReason : byte
{
    Completed = 0,
    Cancelled = 1,
    Collision = 2,
    Invalid = 3,
}
