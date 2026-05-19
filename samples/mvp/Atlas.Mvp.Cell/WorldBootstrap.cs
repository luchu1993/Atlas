// MvpSpace creation moved to Avatar.OnInit (cell): without an Avatar there
// are no observers, and with multi-cellapp a [ModuleInitializer] auto-create
// would land one MvpSpace per cellapp and race the SpaceData NpcCount key.
