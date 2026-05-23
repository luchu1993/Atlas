using System;
using System.Linq;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class ViewRegistry : AtlasEntityViewRegistry<EntityView>
    {
        readonly Action<AvatarView, MvpAvatar> _onOwnerAttached;
        uint _ownerEntityId;
        uint _aoiEnterCount;
        uint _aoiLeaveCount;

        public ViewRegistry(AtlasNetworkManager net, Transform worldRoot,
            AtlasUnityFramePump frame, LabelOverlay labels,
            Action<AvatarView, MvpAvatar> onOwnerAttached)
            : base(net.Session)
        {
            _onOwnerAttached = onOwnerAttached;
            Register<MvpAvatar>(avatar => new AvatarView(avatar, net, worldRoot, frame, labels,
                () => OwnerEntityId));
            Register<MvpNpc>(npc => new NpcView(npc, net, worldRoot, frame, labels,
                () => OwnerEntityId));
            StartTracking();
        }

        public uint OwnerEntityId => _ownerEntityId;
        public uint AoiEnterCount => _aoiEnterCount;
        public uint AoiLeaveCount => _aoiLeaveCount;
        public int NpcViewCount => Views.Count(v => v.Entity is MvpNpc);

        public override void Dispose()
        {
            base.Dispose();
            _ownerEntityId = 0;
        }

        protected override void OnViewAdded(EntityView view, ClientEntity entity)
        {
            ++_aoiEnterCount;
            if (entity.IsOwner && entity is MvpAvatar avatar)
            {
                _ownerEntityId = entity.EntityId;
                _onOwnerAttached((AvatarView)view, avatar);
            }
        }

        protected override void OnViewRemoved(EntityView view, ClientEntity entity)
        {
            ++_aoiLeaveCount;
        }
    }
}
