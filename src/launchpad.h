/*
 * launchpad -- a Novation Launchpad Mini MK2 on the front USB-C, with the
 * Alchemy Lab as the USB host.
 *
 * Shared by copy with smack-alchemy, belt-alchemy and seq-alchemy.
 *
 * The Lab's front port has no VBUS (Hermetic: "identical to ... the Daisy
 * itself, minus VBUS"), so the Launchpad needs 5 V from a powered OTG
 * adapter. libDaisy runs one USB role per boot on that port: while this
 * driver owns it there is no HostLink and no DFU-over-USB; flash through
 * the SD picker, or B3 at power-on.
 *
 * MK2 protocol (Launchpad S / Mini programmer's reference): X-Y layout,
 * grid pad (x, y) = note 16*y + x on channel 1; the right-hand round buttons
 * are x = 8; the top round buttons are CC 104..111. Velocity 127 = press,
 * 0 = release. LED colour = velocity (green << 4) | red | 0x0C, each 0..3.
 *
 * Call Init() once, Poll() every millisecond from the control loop (never
 * from the audio callback). Poll runs the USB host, turns incoming packets
 * into events, and sends only the LEDs that changed, at most one 64-byte
 * USB transfer per call and never waiting for the bus.
 */
#pragma once
#include <stdint.h>

namespace lp
{

/* Colours: red 0..3, green 0..3. */
constexpr uint8_t Col(uint8_t red, uint8_t green)
{
    return (uint8_t)(((green & 3u) << 4) | (red & 3u) | 0x0Cu);
}
constexpr uint8_t kOff      = Col(0, 0);
constexpr uint8_t kRed      = Col(3, 0);
constexpr uint8_t kRedDim   = Col(1, 0);
constexpr uint8_t kGreen    = Col(0, 3);
constexpr uint8_t kGreenDim = Col(0, 1);
constexpr uint8_t kAmber    = Col(3, 3);
constexpr uint8_t kAmberDim = Col(1, 1);
constexpr uint8_t kYellow   = Col(2, 3);
constexpr uint8_t kOrange   = Col(3, 2);

enum class Kind : uint8_t { Grid, Side, Top };

struct Event
{
    Kind    kind;
    uint8_t x;    /* grid 0..7 (left to right), top 0..7 */
    uint8_t y;    /* grid 0..7 (top to bottom), side 0..7 */
    bool    down;
};

void Init();
void Poll(uint32_t now_ms);
bool Connected();
bool PopEvent(Event* e);

/* The LED framebuffer. Changes go out on the next Polls. */
void SetGrid(uint8_t x, uint8_t y, uint8_t col);
void SetSide(uint8_t y, uint8_t col);
void SetTop(uint8_t x, uint8_t col);
void ClearAll();

/* Where the USB host is, for a status light:
 *   0 no device on the wire (no D+ pull-up seen)
 *   1 a device is attached, enumeration in progress or stuck
 *   2 the host gave up (abort / unsupported), or an error fired
 *   3 a MIDI device is active */
uint8_t Stage();

/* Enumeration forensics, latched at the first abort. */
struct Diag
{
    uint8_t  state_before_abort; /* HOST_StateTypeDef reached last, 0..13 */
    uint8_t  enum_at_abort;      /* ENUM_StateTypeDef + 1, 0 = no abort yet */
    bool     vid_ok;             /* the device descriptor said Novation */
    uint16_t aborts;
};
Diag Diagnostics();

/* A text report of the host's state changes and what it read from the
 * device, for writing to the SD card. Returns the length written. */
int Report(char* buf, int cap);

/* Diagnostics for a readout: packets received, transfers sent. */
uint32_t RxCount();
uint32_t TxCount();

} // namespace lp
