// pairing_policy_test.cpp — prove the display-pairing stamp is OFF unless asked for,
// and that every refusal of the `pairing` verb refuses (0.0.267).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -O1 \
//         -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/pairing_policy_test.cpp -o /tmp/pairtest && /tmp/pairtest
//
// It compiles the SAME header the kext compiles.

#include <cstdio>
#include <cstdint>

#include "pairing_policy.h"

static int gFail = 0;
static int gRun  = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-70s got %llu want %llu\n", what, (unsigned long long)got,
                    (unsigned long long)want);
    } else {
        std::printf("ok    %-70s %llu\n", what, (unsigned long long)got);
    }
}

int main()
{
    // --- the per-boot decision -------------------------------------------------
    // The hazard case first: nothing asked, so nothing is stamped.
    uint32_t s = n48_pairing_source(0, 0, kPairingReqNone);
    expect_u("no boot-arg, no verb -> source default-off", s, kPairingSrcDefaultOff);
    expect_u("no boot-arg, no verb -> does NOT stamp", n48_pairing_source_stamps(s), 0);

    s = n48_pairing_source(1, 0, kPairingReqNone);
    expect_u("boot-arg =0 -> source boot-arg-off", s, kPairingSrcBootArgOff);
    expect_u("boot-arg =0 -> does NOT stamp", n48_pairing_source_stamps(s), 0);

    s = n48_pairing_source(1, 2, kPairingReqNone);
    expect_u("boot-arg =2 (not exactly 1) -> does NOT stamp", n48_pairing_source_stamps(s), 0);

    s = n48_pairing_source(1, 1, kPairingReqNone);
    expect_u("boot-arg =1 -> source boot-arg-on", s, kPairingSrcBootArgOn);
    expect_u("boot-arg =1 -> stamps", n48_pairing_source_stamps(s), 1);

    // An absent boot-arg with a stale value in the out-param must not enable.
    s = n48_pairing_source(0, 1, kPairingReqNone);
    expect_u("boot-arg ABSENT with value 1 left in the buffer -> does NOT stamp",
             n48_pairing_source_stamps(s), 0);

    s = n48_pairing_source(0, 0, kPairingReqEnable);
    expect_u("verb 1, no boot-arg -> source verb-on", s, kPairingSrcVerbOn);
    expect_u("verb 1, no boot-arg -> stamps", n48_pairing_source_stamps(s), 1);

    s = n48_pairing_source(1, 1, kPairingReqDisable);
    expect_u("verb 2 overrides boot-arg =1 -> source verb-off", s, kPairingSrcVerbOff);
    expect_u("verb 2 overrides boot-arg =1 -> does NOT stamp", n48_pairing_source_stamps(s), 0);

    s = n48_pairing_source(1, 0, kPairingReqEnable);
    expect_u("verb 1 overrides boot-arg =0 -> stamps", n48_pairing_source_stamps(s), 1);

    s = n48_pairing_source(0, 0, 7);
    expect_u("an unknown request value is treated as no request", s, kPairingSrcDefaultOff);

    // --- the verb --------------------------------------------------------------
    expect_u("arg 0 -> read", n48_pairing_verb_verdict(0, 1, 1, 0), kPairingVerdictRead);
    expect_u("arg 1 before the decision -> enabled", n48_pairing_verb_verdict(1, 0, 0, 0),
             kPairingVerdictEnabled);
    expect_u("arg 1 AFTER the decision -> REFUSED too late",
             n48_pairing_verb_verdict(1, 1, 0, 0), kPairingVerdictTooLate);
    expect_u("arg 1 after the decision and a stamp -> REFUSED too late",
             n48_pairing_verb_verdict(1, 1, 1, 0), kPairingVerdictTooLate);
    expect_u("arg 1 after a withdrawal -> REFUSED too late (never re-stamps)",
             n48_pairing_verb_verdict(1, 1, 1, 1), kPairingVerdictTooLate);
    expect_u("arg 2 before the decision -> disabled", n48_pairing_verb_verdict(2, 0, 0, 0),
             kPairingVerdictDisabled);
    expect_u("arg 2 after the decision, nothing stamped -> disabled",
             n48_pairing_verb_verdict(2, 1, 0, 0), kPairingVerdictDisabled);
    expect_u("arg 2 with keys stamped -> withdraw", n48_pairing_verb_verdict(2, 1, 1, 0),
             kPairingVerdictWithdraw);
    expect_u("arg 2 after a withdrawal -> already withdrawn",
             n48_pairing_verb_verdict(2, 1, 1, 1), kPairingVerdictAlreadyWithdrawn);
    expect_u("arg 3 -> REFUSED bad argument", n48_pairing_verb_verdict(3, 0, 0, 0),
             kPairingVerdictBadArg);
    expect_u("arg 0x100000001 (high bits set) -> REFUSED bad argument",
             n48_pairing_verb_verdict(0x100000001ull, 0, 0, 0), kPairingVerdictBadArg);

    std::printf("\n%d of %d checks passed%s\n", gRun - gFail, gRun, gFail ? " — FAILURES ABOVE" : "");
    return gFail ? 1 : 0;
}
