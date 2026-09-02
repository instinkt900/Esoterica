#!/usr/bin/env bash
#
# Fetch the test content the runtime shakedown needs, into a gitignored directory.
#
# Two things in Phase 8's P8.2 need content that this repository does not ship:
#
#   - A skeletal asset. Data/ has no skeleton, no animation and no animation graph, and its three
#     source meshes are all non-skeletal. The animation graph editor and the ragdoll editor both
#     need a Skeleton resource before they will open.
#   - A map with a dynamic physics body. PBRDemo.map has no physics components at all, so nothing
#     in it can be seen to move. Data/Editor/Floor/Floor.physmesh gives it something to land on.
#
# Everything this script writes lands under Data/PortTests/, which .gitignore covers. Nothing it
# produces is committed, so the third-party assets stay out of the fork's history and Conventions
# rule 6 - do not touch Data/ - still holds for every tracked file.
#
# Usage:
#   Docs/Linux/Scripts/FetchTestAssets.sh          fetch what is missing, rewrite the descriptors
#   Docs/Linux/Scripts/FetchTestAssets.sh -f       re-download even if the source file is present
#   Docs/Linux/Scripts/FetchTestAssets.sh -c       delete Data/PortTests/ and exit
#
# Validate what it writes without a GUI:
#
#   cd Build/Linux_Release
#   ./Esoterica.Applications.ResourceCompiler -compile data://porttests/rig/animation_with_skeleton.skeleton
#
# The compiler exits 1 even on success, so read its log rather than its exit code.
#
#-------------------------------------------------------------------------
# Why FBX and not glTF
#-------------------------------------------------------------------------
#
# The Khronos glTF sample assets would be the tidier source - CC BY 4.0, stable URLs, no FBX
# involved - but glTF skeletal import does not work. Code/EngineTools/Import/Formats/GLTF.cpp:111
# reads:
#
#   EE_ASSERT( pNode->scale[0] != pNode->scale[1] || pNode->scale[1] != pNode->scale[2] );
#
# which asserts that a node's scale is *non*-uniform, and fires on any glTF whose skeleton nodes
# carry a scale. Fox, RiggedFigure, RiggedSimple and CesiumMan all fail on it, at GLTF.cpp:111,
# GLTF.cpp:435 or Transform.h:71. This is platform-neutral upstream code, so it is an upstream
# bug rather than a port defect, and Phase 8 says to record those and move on.
#
# FBX goes through ufbx instead, which is the path Data/'s own meshes already use.

set -euo pipefail

FORCE=0
CLEAN=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f) FORCE=1; shift ;;
        -c) CLEAN=1; shift ;;
        *)  echo "error: unknown argument: $1" >&2; exit 1 ;;
    esac
done

REPO_ROOT="$( cd "$( dirname "${BASH_SOURCE[0]}" )/../../.." && pwd )"
TEST_DATA_DIR="${REPO_ROOT}/Data/PortTests"
RIG_DIR="${TEST_DATA_DIR}/Rig"

if [[ ${CLEAN} -eq 1 ]]; then
    rm -rf "${TEST_DATA_DIR}"
    echo "removed ${TEST_DATA_DIR}"
    exit 0
fi

command -v curl >/dev/null || { echo "error: curl is not installed" >&2; exit 1; }

# assimp's BSD-3-Clause test models. Assets under other terms live in a separate
# test/models-nonbsd directory, which this script deliberately does not touch.
ASSIMP_BASE="https://raw.githubusercontent.com/assimp/assimp/master/test/models/FBX"

mkdir -p "${RIG_DIR}"

#-------------------------------------------------------------------------
# Source assets
#-------------------------------------------------------------------------
# animation_with_skeleton.fbx is the only free rigged asset tried whose skeleton, skinned mesh
# and animation clip all compile. It is a cube on a short bone chain, so it is ugly but complete.
#
# huesitos.fbx is a rigged character, and a better subject for the ragdoll editor. Its skeleton
# and skinned mesh compile; its animation does not, asserting on
# `time <= ( pAnimStack->time_end + Math::Epsilon )` in the ufbx importer. Another upstream bug,
# so this script does not write an .anim for it.

fetch()
{
    local name="$1"
    local dest="${RIG_DIR}/${name}"

    if [[ ${FORCE} -eq 1 || ! -f "${dest}" ]]; then
        echo "fetching ${name}"
        curl -fsSL --retry 3 -o "${dest}.part" "${ASSIMP_BASE}/${name}"
        mv "${dest}.part" "${dest}"
    else
        echo "${name} already present"
    fi
}

fetch animation_with_skeleton.fbx
fetch huesitos.fbx

cat > "${RIG_DIR}/Credits.txt" <<'EOF'
animation_with_skeleton.fbx and huesitos.fbx come from the assimp test model set.
https://github.com/assimp/assimp/tree/master/test/models/FBX

assimp is licensed BSD-3-Clause, and keeps assets under other terms in a separate
test/models-nonbsd directory. These two are from test/models.

Fetched by Docs/Linux/Scripts/FetchTestAssets.sh. Not committed to this repository.
EOF

#-------------------------------------------------------------------------
# Resource descriptors
#-------------------------------------------------------------------------
# Hand-written rather than produced by the editor's importer, so that a session can validate them
# from the command line before opening a GUI.

echo "writing descriptors"

cat > "${RIG_DIR}/Rig.skeleton" <<'EOF'
<Type TypeID="EE::Animation::SkeletonResourceDescriptor" Version="0">
  <Property ID="m_skeletonPath" Value="data://porttests/rig/animation_with_skeleton.fbx" />
  <Property ID="m_previewMesh" Value="data://porttests/rig/rig.skelmesh" />
</Type>
EOF

cat > "${RIG_DIR}/Rig.skelmesh" <<'EOF'
<Type TypeID="EE::Render::SkeletalMeshResourceDescriptor" Version="2">
  <Property ID="m_meshPath" Value="data://porttests/rig/animation_with_skeleton.fbx" />
  <Property ID="m_meshGroup" Value="data://render/meshgroups/default.meshgrp" />
</Type>
EOF

# No m_animationName, so the compiler takes the first clip in the file.
cat > "${RIG_DIR}/Rig.anim" <<'EOF'
<Type TypeID="EE::Animation::AnimationClipResourceDescriptor" Version="0">
  <Property ID="m_animationPath" Value="data://porttests/rig/animation_with_skeleton.fbx" />
  <Property ID="m_skeleton" Value="data://porttests/rig/rig.skeleton" />
</Type>
EOF

cat > "${RIG_DIR}/Character.skeleton" <<'EOF'
<Type TypeID="EE::Animation::SkeletonResourceDescriptor" Version="0">
  <Property ID="m_skeletonPath" Value="data://porttests/rig/huesitos.fbx" />
  <Property ID="m_previewMesh" Value="data://porttests/rig/character.skelmesh" />
</Type>
EOF

cat > "${RIG_DIR}/Character.skelmesh" <<'EOF'
<Type TypeID="EE::Render::SkeletalMeshResourceDescriptor" Version="2">
  <Property ID="m_meshPath" Value="data://porttests/rig/huesitos.fbx" />
  <Property ID="m_meshGroup" Value="data://render/meshgroups/default.meshgrp" />
</Type>
EOF

# A ragdoll needs nothing but a skeleton to be valid - RagdollResourceDescriptor::IsValid checks
# only m_skeleton, and the bodies and collision rules are authored in the ragdoll editor.
cat > "${RIG_DIR}/Character.ragdoll" <<'EOF'
<Type TypeID="EE::Physics::RagdollResourceDescriptor" Version="0">
  <Property ID="m_skeleton" Value="data://porttests/rig/character.skeleton" />
</Type>
EOF

# An empty animation graph. GraphResourceDescriptor::IsValid always returns true and its only
# property is the hidden graph definition, so the editor fills in a default root graph on load.
cat > "${RIG_DIR}/Rig.ag" <<'EOF'
<Type TypeID="EE::Animation::GraphResourceDescriptor" Version="2" />
EOF

#-------------------------------------------------------------------------
# The physics map
#-------------------------------------------------------------------------
# Floor and Skydome are the same two entities PBRDemo.map uses, so the map lights and frames the
# same way a known-good map does. The differences are the static collision mesh on the floor, and
# the dynamic body above it.
#
# A component transform is "rotX,rotY,rotZ,posX,posY,posZ,scale", in degrees and metres, and Z is
# up. Each boulder's sphere body is the root component and the boulder mesh hangs off it as a
# spatial child, so the physics body drives what you see.
#
# Three things this layout is deliberate about:
#
#   - An entity may have exactly one root spatial component. Every other spatial component needs a
#     SpatialParent naming a sibling, or the map compiler reports "Multiple root components found"
#     and drops the whole entity.
#   - The boulders are off to the sides rather than over the origin. The default camera starts near
#     the origin, and a boulder at mesh scale 6 is big enough to land *around* the camera - the
#     first version of this map simulated correctly and looked completely static, because the view
#     was inside the rock.
#   - Three of them, at three heights, spread around the origin. They land about half a second
#     apart, and whichever way the camera happens to face, some are in frame.
#
# The bodies sleep once they settle (worldDef.enableSleep is true), at a resting centre height of
# just over their radius. So a late screenshot showing them on the floor is as good a proof of
# motion as catching the fall.

cat > "${TEST_DATA_DIR}/PhysicsDemo.map" <<'EOF'
<Type TypeID="EE::EntityModel::EntityMapResourceDescriptor" Version="0" />
<CustomData>
  <Entities>
    <Entity Name="Floor">
      <Components>
        <Type TypeID="EE::Render::StaticMeshComponent">
          <Property Path="m_mesh" Value="data://editor/floor/floor.mesh" />
          <Property Path="m_name" Value="Static Mesh Component" />
          <Property Path="m_transform" Value="0,-0,0,0,0,-0.0182571,1" />
        </Type>
        <Type TypeID="EE::Physics::CollisionMeshComponent" SpatialParent="Static Mesh Component">
          <Property Path="m_collisionMesh" Value="data://editor/floor/floor.physmesh" />
          <Property Path="m_name" Value="Floor Collision" />
          <Property Path="m_mobility" Value="Static" />
          <Property Path="m_transform" Value="0,-0,0,0,0,0,1" />
        </Type>
      </Components>
      <ReferencedResources>
        <Resource ID="data://editor/floor/floor.mesh" />
        <Resource ID="data://editor/floor/floor.physmesh" />
      </ReferencedResources>
    </Entity>
    <Entity Name="FallingBoulder1">
      <Components>
        <Type TypeID="EE::Physics::SphereComponent">
          <Property Path="m_name" Value="Boulder Body" />
          <Property Path="m_mobility" Value="Dynamic" />
          <Property Path="m_radius" Value="1.5" />
          <Property Path="m_transform" Value="0,-0,0,0,18,25,1" />
        </Type>
        <Type TypeID="EE::Render::StaticMeshComponent" SpatialParent="Boulder Body">
          <Property Path="m_mesh" Value="data://demo/render/pbr/boulder/boulder.mesh" />
          <Property Path="m_name" Value="Boulder Mesh" />
          <Property Path="m_transform" Value="0,-0,0,0,0,0,3" />
        </Type>
      </Components>
      <ReferencedResources>
        <Resource ID="data://demo/render/pbr/boulder/boulder.mesh" />
      </ReferencedResources>
    </Entity>
    <Entity Name="FallingBoulder2">
      <Components>
        <Type TypeID="EE::Physics::SphereComponent">
          <Property Path="m_name" Value="Boulder Body" />
          <Property Path="m_mobility" Value="Dynamic" />
          <Property Path="m_radius" Value="1.5" />
          <Property Path="m_transform" Value="0,-0,0,18,0,35,1" />
        </Type>
        <Type TypeID="EE::Render::StaticMeshComponent" SpatialParent="Boulder Body">
          <Property Path="m_mesh" Value="data://demo/render/pbr/boulder/boulder.mesh" />
          <Property Path="m_name" Value="Boulder Mesh" />
          <Property Path="m_transform" Value="0,-0,0,0,0,0,3" />
        </Type>
      </Components>
      <ReferencedResources>
        <Resource ID="data://demo/render/pbr/boulder/boulder.mesh" />
      </ReferencedResources>
    </Entity>
    <Entity Name="FallingBoulder3">
      <Components>
        <Type TypeID="EE::Physics::SphereComponent">
          <Property Path="m_name" Value="Boulder Body" />
          <Property Path="m_mobility" Value="Dynamic" />
          <Property Path="m_radius" Value="1.5" />
          <Property Path="m_transform" Value="0,-0,0,-13,-13,45,1" />
        </Type>
        <Type TypeID="EE::Render::StaticMeshComponent" SpatialParent="Boulder Body">
          <Property Path="m_mesh" Value="data://demo/render/pbr/boulder/boulder.mesh" />
          <Property Path="m_name" Value="Boulder Mesh" />
          <Property Path="m_transform" Value="0,-0,0,0,0,0,3" />
        </Type>
      </Components>
      <ReferencedResources>
        <Resource ID="data://demo/render/pbr/boulder/boulder.mesh" />
      </ReferencedResources>
    </Entity>
    <Entity Name="Skydome">
      <Components>
        <Type TypeID="EE::Render::StaticMeshComponent">
          <Property Path="m_mesh" Value="data://editor/skydome/skydome.mesh" />
          <Property Path="m_name" Value="Static Mesh Component" />
          <Property Path="m_transform" Value="0,-0,0,0,0,0,300" />
          <Property Path="m_viewLayers" Value="ForwardShading|GlobalEnvironmentMap" />
        </Type>
        <Type TypeID="EE::Render::DirectionalLightComponent" SpatialParent="Static Mesh Component">
          <Property Path="m_name" Value="Directional Light Component" />
          <Property Path="m_shadowed" Value="True" />
          <Property Path="m_transform" Value="135,-0,0,0,0,0,1" />
        </Type>
      </Components>
      <ReferencedResources>
        <Resource ID="data://editor/skydome/skydome.mesh" />
      </ReferencedResources>
    </Entity>
  </Entities>
</CustomData>
EOF

echo
echo "wrote:"
find "${TEST_DATA_DIR}" -type f | sed "s|^${REPO_ROOT}/|  |" | sort
