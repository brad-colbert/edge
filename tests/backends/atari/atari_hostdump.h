// tests/backends/atari/atari_hostdump.h
// Altirra probe helper: self-dump a memory region to a host file via the H: device.
//
// The Atari .xex probes can't get their results back to the host through Altirra's CLI
// (its /debugcmd: accepts only a single space-free token, so the debugger memory-dump
// commands can't be driven from the command line). Instead a probe calls edge_host_dump()
// to write its snapshot straight to a host file through Altirra's H: device (CIO
// OPEN-for-write / PUT-BINARY / CLOSE on IOCB #1). scripts/altirra_probe.sh launches Altirra
// with `/hdpathrw <hostdir>` so H1: maps to a host directory, then reads the file back.
//
// PROBE-ONLY. Not engine code, not part of any ABI. Needs the OS CIO vector (real Atari /
// Altirra), so it runs on Altirra/hardware, never under mos-sim.

#ifndef EDGE_TEST_ATARI_HOSTDUMP_H
#define EDGE_TEST_ATARI_HOSTDUMP_H

#include <stdint.h>

// Write `len` bytes from `buf` to H: file `name` (e.g. "H1:NSDUMP.BIN"). The name is passed
// WITHOUT the Atari EOL terminator; this appends it. Altirra writes the file lowercased into
// the /hdpathrw directory (e.g. nsdump.bin). Returns the CIO status of the final op
// (1 = success). IOCB #1 is used.
static inline uint8_t edge_host_dump(const char* name, const void* buf, uint16_t len) {
    volatile uint8_t* const ICCMD = (uint8_t*)0x0352;  // IOCB1 command
    volatile uint8_t* const ICBAL = (uint8_t*)0x0354;  // buffer addr lo
    volatile uint8_t* const ICBAH = (uint8_t*)0x0355;  // buffer addr hi
    volatile uint8_t* const ICBLL = (uint8_t*)0x0358;  // length lo
    volatile uint8_t* const ICBLH = (uint8_t*)0x0359;  // length hi
    volatile uint8_t* const ICAX1 = (uint8_t*)0x035A;  // aux1
    volatile uint8_t* const ICAX2 = (uint8_t*)0x035B;  // aux2

    // Copy the name and append the Atari EOL ($9B) terminator CIO expects.
    char fn[28];
    uint8_t i = 0;
    while (name[i] != '\0' && i < (uint8_t)(sizeof(fn) - 2)) { fn[i] = name[i]; ++i; }
    fn[i++] = (char)0x9B;

    const uintptr_t fp = (uintptr_t)fn;
    const uintptr_t dp = (uintptr_t)buf;
    uint8_t st = 0;

    // OPEN for write (AUX1 = 8).
    *ICBAL = (uint8_t)(fp & 0xff); *ICBAH = (uint8_t)((fp >> 8) & 0xff);
    *ICAX1 = 8; *ICAX2 = 0; *ICCMD = 3;
    __asm__ volatile("ldx #$10\n\tjsr $E456\n\tsty %0" : "=r"(st) : : "a", "x", "y");

    // PUT BINARY RECORD (cmd 11): write exactly `len` bytes, no EOL translation.
    *ICBAL = (uint8_t)(dp & 0xff); *ICBAH = (uint8_t)((dp >> 8) & 0xff);
    *ICBLL = (uint8_t)(len & 0xff); *ICBLH = (uint8_t)((len >> 8) & 0xff); *ICCMD = 11;
    __asm__ volatile("ldx #$10\n\tjsr $E456\n\tsty %0" : "=r"(st) : : "a", "x", "y");

    // CLOSE (flushes to the host file).
    *ICCMD = 12;
    __asm__ volatile("ldx #$10\n\tjsr $E456\n\tsty %0" : "=r"(st) : : "a", "x", "y");
    return st;
}

#endif  // EDGE_TEST_ATARI_HOSTDUMP_H
