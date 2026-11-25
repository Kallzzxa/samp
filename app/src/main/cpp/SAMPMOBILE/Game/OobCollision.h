#pragma once

#include <cstdint>
#include <SA.h>

// Maintains repeat-sector injection & CColStore pinning for out-of-bounds
// (|x/y| > 3000) dynamic objects and vehicles so collision remains solid.
void ProcessOobVehicleSectors();
bool PinOobColSlot(sa::CColModel* pCM, int& iPinnedSlot);
void RequestOobColSlot(sa::CColModel* pCM, int iPinnedSlot, uint32_t& dwLastReqTick);
void LogOobColPin(int iModel, int iSlot);
