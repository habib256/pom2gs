// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// SCC 8530 smoke test: exercise the register-pointer protocol, enable local
// loopback (WR14 bit4), transmit bytes, and verify they come back on Rx with
// the correct RR0 status. M7 gate.

#include "Scc8530.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

int main() {
    Scc8530 scc;
    // Channel A command = $C039 (reg 0x9); data = $C03B (reg 0xB).
    const uint8_t ACMD = 0x9, ADATA = 0xB;

    // Set WR14 bit4 (local loopback): write WR0 with pointer=14, then WR14=0x10.
    scc.write(ACMD, 14);       // point at WR14
    scc.write(ACMD, 0x10);     // WR14 = local loopback

    // RR0 before: Tx empty (0x04), no Rx (0x01 clear).
    uint8_t s0 = scc.read(ACMD);
    if (!(s0 & 0x04)) { std::printf("FAIL: Tx-empty not set (RR0=%02X)\n", s0); return 1; }
    if (s0 & 0x01)    { std::printf("FAIL: spurious Rx (RR0=%02X)\n", s0); return 1; }

    const char* msg = "IIgs";
    for (const char* p = msg; *p; ++p) scc.write(ADATA, uint8_t(*p));

    // Each transmitted byte should have looped back to Rx and be readable.
    char got[8] = {0}; int n = 0;
    while (scc.read(ACMD) & 0x01) {          // while Rx char available
        got[n++] = char(scc.read(ADATA));
        if (n >= 7) break;
    }
    bool ok = (n == 4) && std::strcmp(got, msg) == 0;
    std::printf("SCC loopback: sent \"%s\", received \"%s\" (%d bytes)\n", msg, got, n);
    // Host side should also see the transmitted bytes.
    uint8_t hb; int hn = 0; while (scc.hostTx(1, hb)) ++hn;
    std::printf("host Tx drained: %d bytes\n", hn);
    std::printf("%s\n", (ok && hn == 4) ? "OK: SCC loopback works" : "FAIL: SCC loopback");
    return (ok && hn == 4) ? 0 : 1;
}
