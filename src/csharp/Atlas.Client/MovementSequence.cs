namespace Atlas.Client;

public static class MovementSequence
{
    public const uint MaxInputSequenceGap = 256;

    public static uint Delta(uint seq, uint previous) => seq - previous;

    public static bool IsNewer(uint seq, uint previous)
    {
        uint delta = Delta(seq, previous);
        return delta != 0 && delta < 0x80000000u;
    }

    public static bool IsAckStale(uint ackedSeq, uint lastAckSeq, bool hasLastAck) =>
        hasLastAck && !IsNewer(ackedSeq, lastAckSeq) && ackedSeq != lastAckSeq;

    public static bool IsAckStale(uint ackedSeq, uint serverTick, uint lastAckSeq,
                                  uint lastServerTick, bool hasLastAck)
    {
        if (!hasLastAck) return false;
        if (IsNewer(ackedSeq, lastAckSeq)) return false;
        return ackedSeq != lastAckSeq || serverTick <= lastServerTick;
    }

    public static uint SeedNextInputSeqFromAck(uint nextInputSeq, uint ackedSeq)
    {
        uint lastQueuedSeq = nextInputSeq - 1;
        return IsNewer(ackedSeq, lastQueuedSeq) ? ackedSeq + 1 : nextInputSeq;
    }
}
