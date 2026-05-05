namespace Atlas.Components;

// Marker base for client-only state (HUD, prediction, FX). Engine never serialises these.
public abstract class ClientLocalComponent : ClientComponentBase
{
}
