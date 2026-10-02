// pairing_policy.h — whether the display-pairing keys are stamped, and what the
// `pairing` verb may do. 0.0.267, .
//
// WHY OPT-IN. Since 0.0.241 every armed boot stamped IOAccelTypes / IOAccelIndex /
// IOAccelRevision on RDNA4FB, and r79 proved IOKit's IOAccelFindAccelerator then
// resolves our accelerator. traced where WindowServer uses that lookup: ONLY
// when a WindowServer process initialises its displays (CoreDisplay
// CGXMappedDisplayStart). P1 then measured that neither login nor logout restarts
// WindowServer (pid 201 throughout). So on a normal boot the stamp does nothing —
// but if WindowServer crashes or is killed on an ARMED boot, the new process would
// pair the display with an accelerator that cannot run the compositor's shaders,
// login window included. The stamp is therefore OFF unless something asks for it.
//
// A PURE FUNCTION, as rootwrite_guards.h is, so tests/pairing_policy_test.cpp drives
// every branch — in particular the refusals — with the identical code the kext runs.
//
// The decision is taken ONCE per boot, at Apple's accelerator-started callback
// (kReqAccelStarted), which is the only caller of the stamp. A verb that arrives
// after that point cannot turn the stamp on; it can still withdraw keys it stamped.

#ifndef NAVI48_PAIRING_POLICY_H
#define NAVI48_PAIRING_POLICY_H

#include <stdint.h>

// What the verb asked for (the verb's argument, remembered until the decision).
enum PairingRequest {
    kPairingReqNone    = 0,
    kPairingReqEnable  = 1,
    kPairingReqDisable = 2,
};

// Where the per-boot decision came from.
enum PairingSource {
    kPairingSrcDefaultOff = 0,   // no boot-arg, no verb: OFF (0.0.267)
    kPairingSrcBootArgOn  = 1,   // navi48-display-pairing=1
    kPairingSrcBootArgOff = 2,   // navi48-display-pairing present with any other value
    kPairingSrcVerbOn     = 3,   // `pairing 1` before fire
    kPairingSrcVerbOff    = 4,   // `pairing 2` before fire
};

// The verb's outcome.
enum PairingVerdict {
    kPairingVerdictRead             = 0,  // argument 0: report only
    kPairingVerdictEnabled          = 1,  // `pairing 1` accepted, before the decision
    kPairingVerdictDisabled         = 2,  // `pairing 2` accepted, nothing was stamped
    kPairingVerdictWithdraw         = 3,  // `pairing 2` and the keys ARE stamped: withdraw them
    kPairingVerdictTooLate          = 4,  // `pairing 1` REFUSED: the decision has been taken
    kPairingVerdictBadArg           = 5,  // any other argument: REFUSED
    kPairingVerdictAlreadyWithdrawn = 6,  // `pairing 2` after a withdrawal: nothing to do
};

// The verb wins over the boot-arg (it is the per-boot test lever, as `fire 1|2` and
// `bootchain <mode>` are). Only the exact value 1 enables through the boot-arg, so a
// typo cannot turn the stamp on.
static inline uint32_t n48_pairing_source(int bootArgPresent, uint32_t bootArgValue,
                                          uint32_t verbRequest)
{
    if (verbRequest == kPairingReqEnable)  return kPairingSrcVerbOn;
    if (verbRequest == kPairingReqDisable) return kPairingSrcVerbOff;
    if (bootArgPresent) return bootArgValue == 1 ? kPairingSrcBootArgOn : kPairingSrcBootArgOff;
    return kPairingSrcDefaultOff;
}

static inline int n48_pairing_source_stamps(uint32_t source)
{
    return source == kPairingSrcBootArgOn || source == kPairingSrcVerbOn;
}

// decided: the accelerator-started callback has already taken the decision this boot.
// stamped: the keys were written (and read back) by us this boot.
// withdrawn: a previous `pairing 2` removed them.
static inline uint32_t n48_pairing_verb_verdict(uint64_t arg, int decided, int stamped,
                                                int withdrawn)
{
    if (arg == 0) return kPairingVerdictRead;
    if (arg == 1) return decided ? kPairingVerdictTooLate : kPairingVerdictEnabled;
    if (arg == 2) {
        if (withdrawn) return kPairingVerdictAlreadyWithdrawn;
        return stamped ? kPairingVerdictWithdraw : kPairingVerdictDisabled;
    }
    return kPairingVerdictBadArg;
}

static inline const char *n48_pairing_source_name(uint32_t source)
{
    switch (source) {
    case kPairingSrcDefaultOff: return "OFF by default (0.0.267: opt-in)";
    case kPairingSrcBootArgOn:  return "ON by boot-arg navi48-display-pairing=1";
    case kPairingSrcBootArgOff: return "OFF by boot-arg navi48-display-pairing (value not 1)";
    case kPairingSrcVerbOn:     return "ON by the `pairing 1` verb";
    case kPairingSrcVerbOff:    return "OFF by the `pairing 2` verb";
    default:                    return "?";
    }
}

static inline const char *n48_pairing_verdict_name(uint32_t verdict)
{
    switch (verdict) {
    case kPairingVerdictRead:             return "read";
    case kPairingVerdictEnabled:          return "ENABLED for this boot (stamps at the accelerator-started callback)";
    case kPairingVerdictDisabled:         return "DISABLED for this boot (nothing was stamped)";
    case kPairingVerdictWithdraw:         return "WITHDRAW the stamped keys";
    case kPairingVerdictTooLate:          return "REFUSED: the stamp decision was already taken at accelerator start";
    case kPairingVerdictBadArg:           return "REFUSED: argument must be 0 (read), 1 (enable) or 2 (disable/withdraw)";
    case kPairingVerdictAlreadyWithdrawn: return "already withdrawn: nothing to do";
    default:                              return "?";
    }
}

#endif // NAVI48_PAIRING_POLICY_H
