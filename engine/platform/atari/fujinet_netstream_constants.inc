; engine/platform/atari/fujinet_netstream_constants.inc
;
; Stage 9E: EDGE-owned, compact constants for the translated FujiNet Netstream
; handler. This is a deliberately MINIMAL set covering only the registers and
; OS database locations the handler actually touches -- it is NOT a wholesale
; copy of the upstream CA65 hardware.inc / kerneldb.inc includes.
;
; Atari-specific addresses and the rationale for them live here in the platform
; layer (never in the generic engine headers).
;
; Numeric syntax is GNU/llvm (0x...), not CA65 ($...).
;
; This file is included with the C preprocessor; it must only be pulled into
; sources compiled as `assembler-with-cpp` (i.e. a .S source, or a .s source
; built with an explicit `-x assembler-with-cpp`).

#ifndef ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_CONSTANTS_INC
#define ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_CONSTANTS_INC

;==========================================================================
; POKEY (serial + audio)
#define NS_POKEY_BASE 0xD200    // AUDF1; pokey_init block written from here
#define NS_AUDF3      0xD204
#define NS_AUDF4      0xD206
#define NS_AUDCTL     0xD208
#define NS_SERIN      0xD20D    // read: serial input
#define NS_SEROUT     0xD20D    // write: serial output
#define NS_IRQEN      0xD20E    // write: IRQ enable
#define NS_IRQST      0xD20E    // read: IRQ status
#define NS_SKCTL      0xD20F    // write: serial port control
#define NS_SKSTAT     0xD20F    // read: serial port status

;==========================================================================
; PIA
#define NS_PACTL      0xD302    // motor line assert/deassert

;==========================================================================
; ANTIC
#define NS_VCOUNT     0xD40B    // scanline/2 counter (PAL/NTSC detection)

;==========================================================================
; OS database (page 0/2 shadows + serial IRQ vectors)
#define NS_POKMSK     0x0010    // POKEY IRQ mask shadow
#define NS_SSKCTL     0x0232    // SKCTL shadow
#define NS_VSERIN     0x020A    // serial input ready vector (vseror/vseroc follow)
#define NS_VIMIRQ     0x0216    // immediate IRQ vector
; cc65 fastcall arg pointer (c_sp) lives at 0x82. It is NOT used by EDGE: the
; llvm-mos calling convention differs, so init arg marshaling is done in C
; (edge_ns_init_marshal, fujinet_netstream_realtime_abi.h) which writes the staged
; init inputs below; the raw _ns_init_netstream entry reads them.

;==========================================================================
; SIO
#define NS_SIOV       0xE459    // SIO vector (DCB-driven I/O)
; Device Control Block (used by the real init path)
#define NS_DDEVIC     0x0300
#define NS_DUNIT      0x0301
#define NS_DCOMND     0x0302
#define NS_DSTATS     0x0303
#define NS_DBUFLO     0x0304
#define NS_DBUFHI     0x0305
#define NS_DTIMLO     0x0306
#define NS_DBYTLO     0x0308
#define NS_DBYTHI     0x0309
#define NS_DAUX1      0x030A
#define NS_DAUX2      0x030B

;==========================================================================
; Handler tunables (buffer sizes / payload limits)
#define NS_INPUT_BUFSIZE  0x80   // internal 128-byte input buffer
#define NS_OUTPUT_BUFSIZE 128    // 128-byte output ring
#define NS_HOST_MAX       61     // max hostname length before forced NUL
#define NS_PAYLOAD_MAX    64     // init payload buffer size

#endif // ENGINE_PLATFORM_ATARI_FUJINET_NETSTREAM_CONSTANTS_INC
