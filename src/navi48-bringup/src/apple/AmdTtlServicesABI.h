// AmdTtlServicesABI.h — the binary interface Apple's accelerator expects from
// its hardware library, reconstructed from com.apple.kext.AMDRadeonX6000HWLibs
// (Tahoe 26.6.2, __ZTV14AmdTtlServices @ 0xc7c28f0).
//
// WHY THIS EXISTS
// ---------------
// AMDRadeonX6000 (Apple's accelerator) is generation-agnostic and links only
// against IOKit. Everything it needs from silicon it gets through ONE pointer,
// obtained as hwServices->getTtl() and stored at AMDHardware+0x338. Apple's
// implementation of that interface (AmdTtlServices, in HWLibs) tops out at
// Navi 23 and cannot drive gfx1201 — but the interface itself is generic.
//
// So we implement it ourselves, backed by the bring-up kext, and hand Apple's
// accelerator our object instead of Apple's. See notes/APPLE-DRIVER-VERDICT.md.
//
// LAYOUT IS LOAD-BEARING
// ----------------------
// The order below IS the vtable. It is transcribed from notes/re/ttl-vtable.txt,
// which was decoded from the shipping binary. One inserted, removed or reordered
// method silently misroutes every call after it. Do not "tidy" this list. The
// slot number in each comment is checked by a static assert in Navi48Ttl.cpp.
//
// Slots 0 and 1 are the complete and deleting destructors: AmdTtlServices is a
// plain C++ class with a virtual destructor and no IOKit base, so `virtual ~T()`
// occupies exactly those two slots. Do NOT derive this from OSObject.
//
// RETURN TYPES were not recoverable from the mangled names (Itanium C++ does not
// encode them). They are inferred from call sites and marked (inferred) where
// the inference is from a single site. On x86-64 every one of these returns in
// %eax/%rax, so a wrong *width* is harmless; a wrong *meaning* is not.
//   * initialize / powerUp:              0 = success. Confirmed — the accelerator
//                                        logs "Error code: 0x%08x" on non-zero.
//   * queryHwBlockRegisterBase:          0 = success. Confirmed at 19 call sites
//                                        (`testl %eax,%eax; jne fail`).
//   * getHwipSegmentCount:               the count itself; 0 means "give up".
//
// 16 of the 43 slots are actually called by the accelerator. They are marked
// [USED] with their callers. The other 27 may be left returning kTtlUnsupported
// until something proves otherwise.

#pragma once
#include <stdint.h>

class IOMemoryDescriptor;
class AMDFirmwareDirectory;

// ---------------------------------------------------------------- status codes
enum : uint32_t {
    kTtlOk          = 0,
    kTtlError       = 1,
    kTtlUnsupported = 2,
};

// ------------------------------------------------------- recovered ABI structs
//
// _TtlLibraryInitializationInput — built on the stack by
// AMDRadeonX6000_AMDRTHardware::initializeTtl(_GART_PARAMETERS*) @ 0xbe19a6c.
// Field names at 0x20/0x28/0x30 are Apple's own, taken verbatim from the log
// line it prints immediately before calling initialize().
struct AccelGmmCallbacks {                 // &RTHardware+0x20690
    void*    context;                      // the RTHardware instance
    uint32_t (*allocateVram)(void* ctx, const void* in, void* out);
    uint32_t (*releaseVram)(void* ctx, void* in);
};
struct AccelEventLogCallbacks {            // &RTHardware+0x206a8
    void*    context;                      // the RTHardware instance
    void*    reserved;
    uint32_t (*addEvent)(void* ctx, void* a, void* in);
    void*    eventLog;                     // AMDEventLogManager::newEventLog("TTL", ...)
};
struct TtlLibraryInitializationInput {     // 0x38 bytes
    AccelGmmCallbacks*      pGmmCallbacks;              // 0x00
    AccelEventLogCallbacks* pEventLogCallbacks;         // 0x08
    uint64_t                reserved0;                  // 0x10  zeroed by caller
    uint64_t                reserved1;                  // 0x18  zeroed by caller
    uint64_t                nonlocalMemSizeLimitBytes;  // 0x20  *(_GART_PARAMETERS*)
    void*                   pDoorbellBase;              // 0x28
    uint32_t                doorbellApertureSizeInBytes;// 0x30
    uint32_t                pad;                        // 0x34
};

// hwblock_type — Apple's IP-block id for queryHwBlockRegisterBase.
// Observed values at the configureRegisterBases call sites: 0x07, 0x0b, 0x42,
// 0x4b. 0x0b is GC, which matches amdgpu's GC_HWID == 11, so this is almost
// certainly AMD's shared HWIP enum that amdgpu also derives from. Treat the
// mapping as UNCONFIRMED until each id is observed against a known block.
typedef uint32_t hwblock_type;

// Opaque until a call site forces us to define them. Every one of these is
// passed by pointer, so an incomplete type is safe.
struct TtlRtsInfo; struct GmmLocalMemoryInfo; struct GmmNonLocalMemoryInfo;
struct GcHardwareInfo; struct GpuMemType;

// Four u64s. AMDHardware::initHWInfo zeroes a 32-byte local, calls slot 16, and
// on success copies the four values to Hardware+0x190/0x198/0x1a0/0x1a8.
//
// Those land at 0xc0/0xc8/0xd0/0xd8 of the 0x204-byte _sAMD_GET_HW_INFO_VALUES
// that AMDAccelDevice::getHardwareInfo memcpys out, and getHardwareInfo REJECTS
// the device if any of the four is zero:
//
//   AMDAccelDevice::getHardwareInfo - Invalid values -
//       refClkFreq: 0x0 sysClkFreq: 0x0 memClkFreq: 0x0 cgRefClkFreq: 0x0
//
// The names come from that very message, in that order.
//
// UNITS ARE NOT ESTABLISHED. Nothing in AMDHardware reads these back, so there
// is no consumer to infer them from; the only hard requirement found is
// non-zero. They are filled in kHz below and logged, so the value Apple then
// reports in its own "Core Clock(MHz)" counter can be compared against the
// truth and the scale corrected. Do not assume kHz is right.
struct GpuClkInfo {
    uint64_t refClkFreq;
    uint64_t sysClkFreq;
    uint64_t memClkFreq;
    uint64_t cgRefClkFreq;
};
// Recovered from AMDVCN2DecChannel::initializeStart (X6000 @ 0xbe48040), which
// is the clearest caller of slots 18 and 19:
//
//   TtlCommandInfoOutput info = {};
//   ttl->queryCommandInfo(queue, instance, cmd, &info);     // *0x90 -> slot 18
//   TtlBuildCommandInfo b = {};
//   b.word0 = info.word0;                                   // dword 0 carried over
//   b.one   = 1;
//   ttl->buildCommand(queue, instance, &b, dest);           // *0x98 -> slot 19
//   return info.sizeBytes >> 2;                             // BYTES -> dwords
//
// So dword 0 identifies the command to the builder, and dword 1 is the size of
// the command IN BYTES — the caller shifts it right by two to get a dword count
// and uses that to advance its ring. Anything past dword 2 is untouched by this
// caller; the struct is over-sized deliberately rather than assuming 12 bytes is
// all there is.
struct TtlCommandInfoOutput {
    uint32_t word0;        // opaque; handed back to buildCommand
    uint32_t sizeBytes;    // command length in bytes
    uint32_t word2;
    uint32_t reserved[13];
};

struct TtlBuildCommandInfo {
    uint32_t word0;        // = TtlCommandInfoOutput::word0
    uint32_t pad0;
    uint32_t one;          // the caller sets this to 1
    uint32_t pad1;
    uint64_t reserved[2];
};
struct CrossArchIriIsFwLoadedInput; struct CrossArchIriIsFwLoadedOutput;
struct AmdTtlCollectDiagInfoInput; struct AmdTtlCollectDiagInfoOutput;
struct AmdFwAttestationInput; struct AmdFwAttestationOutput;
struct SwipQueueInParams; struct SwipQueueOutParams; struct SwipEngineState;
struct TtlDpmTableInfo;
struct BgdSecurityHdcpInput;  struct BgdSecurityHdcpOutput;
struct BgdSecurityTopoInput;  struct BgdSecurityTopoOutput;
struct BgdSecurityAucInput;   struct BgdSecurityAucOutput;
struct BgdSecurityFpInput;    struct BgdSecurityFpOutput;
struct BgdSecurityXgmiInput;  struct BgdSecurityXgmiOutput;
struct BgdSecurityRapOutput;

typedef uint32_t AmdSwipQueueType;
typedef uint32_t AmdTtlCommandType;
typedef uint32_t AmdTtlEngineEvent;
typedef uint32_t AmdMesRequestType;
typedef uint32_t AmdDpmClockType;

// --------------------------------------------------------------- the interface

// EVERY method below has an inline body. That is not style — it is a load-bearing
// linkage requirement. A virtual declared here but defined nowhere leaves an
// unresolved symbol in the kext; the kernel linker then refuses the whole kext and
// OpenCore skips it with no message in any log. Version 0.0.39 failed to load for
// exactly this reason: ~AmdTtlServicesABI() had no definition. The defaults return
// kTtlUnsupported so an unimplemented slot degrades to "not supported" rather than
// to a wild jump. Navi48Ttl overrides all 43 regardless.
class AmdTtlServicesABI {
public:
    virtual ~AmdTtlServicesABI() {}                                                       // slots 0,1

    virtual uint32_t getTtlRtsInfo(TtlRtsInfo*) { return kTtlUnsupported; }                                        // 2
    virtual uint32_t getLocalMemoryInfo(GmmLocalMemoryInfo*) { return kTtlUnsupported; }                           // 3
    virtual uint32_t getNonLocalMemoryInfo(GmmNonLocalMemoryInfo*) { return kTtlUnsupported; }                     // 4
    virtual uint32_t getSwipErrorString(char*, unsigned long) { return kTtlUnsupported; }                          // 5  [USED] initializeTtl
    virtual uint32_t initialize(TtlLibraryInitializationInput*) { return kTtlUnsupported; }                        // 6  [USED] initializeTtl — THE failure point
    virtual uint32_t uninitialize() { return kTtlUnsupported; }                                                    // 7
    virtual uint32_t setFirmwareDirectory(AMDFirmwareDirectory*) { return kTtlUnsupported; }                       // 8
    virtual uint32_t powerUp() { return kTtlUnsupported; }                                                         // 9  [USED] Hardware/RTHardware::willWake
    virtual uint32_t powerDown() { return kTtlUnsupported; }                                                       // 10
    virtual uint32_t queryHwBlockRegisterBase(hwblock_type, unsigned char instance,
                                              unsigned int segment,
                                              unsigned int* outBase) { return kTtlUnsupported; }                   // 11 [USED] x19 — HWMemory::init, DCNDisplay::init,
                                                                                        //              configureRegisterBases, MMHub::fillVMRegisters
    virtual unsigned int getHwipSegmentCount() const { return 0; }                                   // 12 [USED] x11
    virtual uint32_t queryCSBInfo(unsigned long long*, unsigned int*) { return kTtlUnsupported; }                   // 13 [USED] Hardware::initHWInfo
    virtual uint32_t queryGcHardwareInfo(GcHardwareInfo*) { return kTtlUnsupported; }                              // 14 [USED] GFX10Hardware::setupAndInitializeHWCapabilities
    virtual uint32_t queryGpuMemType(GpuMemType*) { return kTtlUnsupported; }                                      // 15 [USED] Hardware::initHWInfo
    virtual uint32_t queryGpuClkInfo(GpuClkInfo*) { return kTtlUnsupported; }                                      // 16 [USED] Hardware::initHWInfo
    virtual uint32_t getGartTableAddress(void**, unsigned long long*) { return kTtlUnsupported; }                  // 17
    virtual uint32_t queryCommandInfo(AmdSwipQueueType, unsigned int,
                                      AmdTtlCommandType, TtlCommandInfoOutput*) { return kTtlUnsupported; }        // 18
    virtual uint32_t buildCommand(AmdSwipQueueType, unsigned int,
                                  TtlBuildCommandInfo*, void*) { return kTtlUnsupported; }                         // 19
    virtual uint32_t submitFrame(AmdSwipQueueType, unsigned int, void*) { return kTtlUnsupported; }                // 20
    virtual uint32_t queryFwLoadingStatus(CrossArchIriIsFwLoadedInput*,
                                          CrossArchIriIsFwLoadedOutput*) { return kTtlUnsupported; }               // 21 [USED] Hardware::crossArchIRICall
    virtual uint32_t addGartSaveRestoreRange(unsigned long long, unsigned long long) { return kTtlUnsupported; }   // 22
    virtual uint32_t removeGartSaveRestoreRange(unsigned long long) { return kTtlUnsupported; }                    // 23
    virtual uint32_t mapToGart(unsigned long long, IOMemoryDescriptor*) { return kTtlUnsupported; }                // 24
    virtual uint32_t unmapFromGart(unsigned long long, IOMemoryDescriptor*) { return kTtlUnsupported; }            // 25
    virtual uint32_t hdcp_services(uint32_t, BgdSecurityHdcpInput*, BgdSecurityHdcpOutput*) { return kTtlUnsupported; }   // 26
    virtual uint32_t display_topology_services(uint32_t, BgdSecurityTopoInput*,
                                               BgdSecurityTopoOutput*) { return kTtlUnsupported; }                 // 27
    virtual uint32_t auc_services(uint32_t, BgdSecurityAucInput*, BgdSecurityAucOutput*) { return kTtlUnsupported; }      // 28
    virtual uint32_t fp_services(uint32_t, BgdSecurityFpInput*, BgdSecurityFpOutput*) { return kTtlUnsupported; }         // 29
    virtual uint32_t xgmi_services(uint32_t, BgdSecurityXgmiInput*, BgdSecurityXgmiOutput*) { return kTtlUnsupported; }   // 30 [USED] enablePeerAccess, refreshXgmiInfo
    virtual uint32_t rap_services(uint32_t, BgdSecurityRapOutput*) { return kTtlUnsupported; }                     // 31
    virtual uint32_t collectEngineDiagInfo(AmdTtlCollectDiagInfoInput*,
                                           AmdTtlCollectDiagInfoOutput*) { return kTtlUnsupported; }               // 32
    virtual uint32_t queryFwAttestationInfo(AmdFwAttestationInput*,
                                            AmdFwAttestationOutput*) { return kTtlUnsupported; }                   // 33
    virtual uint32_t queryEngineQueueCount(AmdSwipQueueType, unsigned int*) { return kTtlUnsupported; }            // 34
    virtual uint32_t notifyEngine(AmdSwipQueueType, unsigned int, AmdTtlEngineEvent) { return kTtlUnsupported; }   // 35
    virtual uint32_t startEngineQueue(AmdSwipQueueType, unsigned int,
                                      SwipQueueInParams*, SwipQueueOutParams*) { return kTtlUnsupported; }         // 36 [USED] GFX10SDMAEngine::start, GFX10KIQHWChannel::startKIQ
    virtual uint32_t stopEngineQueue(AmdSwipQueueType, unsigned int) { return kTtlUnsupported; }                   // 37 [USED] GFX10PM4Engine::initGraphicsMQD/doStop, SDMAEngine::start/stop
    virtual uint32_t queryEngineQueueState(AmdSwipQueueType, unsigned int,
                                           SwipEngineState*) { return kTtlUnsupported; }                           // 38 [USED] HWEngine::queryQueueState
    virtual uint32_t resetEngineQueue(AmdSwipQueueType, unsigned int) { return kTtlUnsupported; }                  // 39 [USED] HWEngine::reset
    virtual uint32_t notifyHardwareState(bool) { return kTtlUnsupported; }                                         // 40 [USED] GFX10KIQHWChannel::startKIQ
    virtual uint32_t sendRequestToMES(AmdMesRequestType, void*) { return kTtlUnsupported; }                        // 41
    virtual uint32_t queryDpmTableInfo(TtlDpmTableInfo*, AmdDpmClockType) { return kTtlUnsupported; }              // 42
};

// The accelerator indexes this vtable by byte offset. If the compiler ever lays
// it out differently from the table we measured, every call lands on the wrong
// method and the symptom is an unexplainable hang, so verify at build time.
static_assert(sizeof(void*) == 8, "vtable slot arithmetic assumes 64-bit");
