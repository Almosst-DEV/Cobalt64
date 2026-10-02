//
//  amdgpu_discovery.h — fills mac-amdgpu's IPBaseTable from our on-die IP
//  discovery parser (RDNA4FB's IpDiscovery). Replaces the reference's own
//  discovery reader (amdgpu_discovery.cpp), which we don't need.
//
#pragma once
#include "amdgpu_ip.h"
class IpDiscovery;

namespace amdgpu {
// Returns true when every block the bring-up needs (GC, MP0, MP1, MMHUB,
// OSSSYS, HDP, SDMA0, NBIO) resolved. SDMA1 and GMC are optional.
bool fill_ip_base_table(const IpDiscovery &disc, IPBaseTable &table);
const char *ip_block_name(IPBlock b);
}
