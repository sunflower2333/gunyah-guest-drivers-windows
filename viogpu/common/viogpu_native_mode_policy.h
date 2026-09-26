#pragma once

/* Constraint enumeration may use UNPINNED/NOTSPECIFIED. An actual committed
 * native mode may not. No matched producer and smooth-update profile exists
 * yet, so native mode commit and active-path update both require identity.
 * Keep this distinct from DriverRotation, which describes implementation
 * placement rather than a rotation capability. */
static inline bool VioGpuNativeCommittedRotationSupported(unsigned int vidPnRotation)
{
    return vidPnRotation == 1U; /* D3DKMDT_VPPR_IDENTITY */
}
