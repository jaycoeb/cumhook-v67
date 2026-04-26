#include "includes.h"

Bones g_bones{};;

bool Bones::setup( Player* player, BoneArray* out, LagRecord* record ) {
	// if the record isnt setup yet.
	if( !record->m_setup ) {
		// run setupbones rebuilt.
		if( !BuildBones( player, 0x7FF00, record->m_bones, record ) )
			return false;

		// we have setup this record bones.
		record->m_setup = true;
	}

	// record is setup.
	if( out && record->m_setup )
		std::memcpy( out, record->m_bones, sizeof( BoneArray ) * 128 );

	return true;
}

bool Bones::BuildBones(Player* target, int mask, BoneArray* out, LagRecord* record) {
    if (!out)
        return false;

    vec3_t         pos[128];
    quaternion_t   q[128];

    vec3_t backup_origin;
    ang_t  backup_angles;
    float  backup_poses[24];
    C_AnimationLayer backup_layers[13];

    vec3_t backup_velocity;
    int    backup_flags;
    int    backup_eflags;

    CStudioHdr* hdr = target->GetModelPtr();
    if (!hdr)
        return false;

    CBoneAccessor* accessor = &target->m_BoneAccessor();
    if (!accessor)
        return false;

    BoneArray* backup_matrix = accessor->m_pBones;
    if (!backup_matrix)
        return false;

    auto bSkipAnimationFrame = *reinterpret_cast<int*>(uintptr_t(target) + 0x260);
    *reinterpret_cast<int*>(uintptr_t(target) + 0x260) = NULL;

    // backup
    backup_origin = target->GetAbsOrigin();
    backup_angles = target->GetAbsAngles();
    backup_velocity = target->m_vecAbsVelocity();
    backup_flags = target->m_fFlags();
    backup_eflags = target->m_iEFlags();

    target->GetPoseParameters(backup_poses);
    target->GetAnimLayers(backup_layers);

    matrix3x4_t transform;
    math::AngleMatrix(record->m_abs_ang, record->m_pred_origin, transform);

    target->AddEffect(EF_NOINTERP);

    target->SetAbsOrigin(record->m_pred_origin);
    target->SetAbsAngles(record->m_abs_ang);

    target->m_vecAbsVelocity() = record->m_pred_velocity;
    target->m_fFlags() = record->m_pred_flags;

    target->m_iEFlags() &= ~0x1000;

    target->SetPoseParameters(record->m_poses);
    target->SetAnimLayers(record->m_layers);

    accessor->m_pBones = out;

    m_running = true;

    target->StandardBlendingRules(hdr, pos, q, record->m_pred_time, mask);

    uint8_t computed[128] = {};
    target->BuildTransformations(hdr, pos, q, transform, mask, computed);

    accessor->m_pBones = backup_matrix;

    // restore
    target->SetAbsOrigin(backup_origin);
    target->SetAbsAngles(backup_angles);
    target->m_vecAbsVelocity() = backup_velocity;
    target->m_fFlags() = backup_flags;
    target->m_iEFlags() = backup_eflags;

    target->SetPoseParameters(backup_poses);
    target->SetAnimLayers(backup_layers);

    m_running = false;

    *reinterpret_cast<int*>(uintptr_t(target) + 0x260) = bSkipAnimationFrame;

    return true;
}