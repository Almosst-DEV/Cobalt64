#include "amdgpu_discovery.h"
#include "amdgpu_log.h"
#include "../ipdiscovery.hpp"

namespace amdgpu {

namespace {
// Linux soc15_hw_ip.h hardware ids (same values the reference's HWID enum uses).
constexpr uint16_t kHwMp1 = 1, kHwGc = 11, kHwMmhub = 34, kHwOsssys = 40, kHwHdp = 41,
                   kHwSdma0 = 42, kHwSdma1 = 43, kHwNbif = 108, kHwMp0 = 255;
struct Map { IPBlock block; uint16_t hwId; uint8_t instance; bool required; };
const Map kMap[] = {
    { IPBlock::GC,     kHwGc,     0, true  },
    { IPBlock::HDP,    kHwHdp,    0, true  },
    { IPBlock::SDMA0,  kHwSdma0,  0, true  },
    { IPBlock::SDMA1,  kHwSdma1,  0, false },
    { IPBlock::MP0,    kHwMp0,    0, true  },
    { IPBlock::MP1,    kHwMp1,    0, true  },
    { IPBlock::NBIO,   kHwNbif,   0, true  },
    { IPBlock::OSSSYS, kHwOsssys, 0, true  },
    { IPBlock::MMHUB,  kHwMmhub,  0, true  },
};
}

const char *ip_block_name(IPBlock b) {
    switch (b) {
    case IPBlock::GC: return "GC";       case IPBlock::HDP: return "HDP";
    case IPBlock::SDMA0: return "SDMA0"; case IPBlock::SDMA1: return "SDMA1";
    case IPBlock::MP0: return "MP0";     case IPBlock::MP1: return "MP1";
    case IPBlock::NBIO: return "NBIO";   case IPBlock::OSSSYS: return "OSSSYS";
    case IPBlock::GMC: return "GMC";     case IPBlock::MMHUB: return "MMHUB";
    default: return "?";
    }
}

bool fill_ip_base_table(const IpDiscovery &disc, IPBaseTable &table) {
    table = IPBaseTable();
    bool ok = true;
    for (const Map &m : kMap) {
        IpDiscovery::IpEntry e;
        bool found = disc.findIp(m.hwId, m.instance, e);
        if (!found && m.block == IPBlock::SDMA1) found = disc.findIp(kHwSdma0, 1, e);   // second instance under SDMA0's id
        if (!found) {
            if (m.required) { UCODE_LOG("discovery: required block %s (hwid %u) missing", ip_block_name(m.block), m.hwId); ok = false; }
            continue;
        }
        const int n = e.numBases < IPBaseTable::kMaxBaseSegments ? e.numBases : IPBaseTable::kMaxBaseSegments;
        for (int i = 0; i < n; i++) table.setBase(m.block, i, e.bases[i]);
        table.version[(int)m.block] = IPVersion{ e.major, e.minor, e.revision };
    }
    return ok;
}

} // namespace amdgpu
