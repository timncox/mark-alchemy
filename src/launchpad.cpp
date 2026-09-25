/* launchpad -- see launchpad.h. */
#include "launchpad.h"

#include <stdio.h>
#include <string.h>

#include "daisy_core.h"
#include "hid/usb_host.h"
#include "usbh_midi.h"

extern "C" USBH_HandleTypeDef hUsbHostHS; /* libDaisy's host handle */

namespace lp
{

namespace
{

constexpr int kGrid  = 64;
constexpr int kSide0 = 64;
constexpr int kTop0  = 72;
constexpr int kCells = 80;

daisy::USBHostHandle g_usbh;
bool                 g_ready     = false; /* MIDI class active, pipes open */
bool                 g_failed    = false; /* abort, unsupported or error seen */
bool                 g_need_init = false; /* send reset + layout next */

uint8_t g_want[kCells];
uint8_t g_sent[kCells];
int     g_scan = 0; /* where the next diff scan starts, for fairness */

/* USB transfers come from the host DMA: keep the buffer out of DTCM. */
uint8_t DMA_BUFFER_MEM_SECTION g_tx[64];

constexpr int kQueue = 64;
Event         g_q[kQueue];
volatile int  g_qhead = 0, g_qtail = 0;

uint32_t g_rx_count = 0, g_tx_count = 0;

Diag    g_diag     = {0, 0, false, 0};
uint8_t g_last_gs  = 0;
uint8_t g_last_es  = 0;

/* Transition trace: (ms, host state, enum state). */
struct Tr { uint32_t t; uint8_t gs, es; };
constexpr int kTrace = 96;
Tr      g_tr[kTrace];
int     g_tr_n = 0;

/* What the host held when it reached CHECK_CLASS the first time. */
bool     g_cc_seen = false;
uint8_t  g_cc_dev[18];
uint8_t  g_cc_cfg[64];
uint16_t g_cc_total = 0;
uint8_t  g_cc_nif = 0, g_cc_if0c = 0, g_cc_if1c = 0, g_cc_if1s = 0, g_cc_if1ep = 0;
uint16_t g_cc_vid = 0, g_cc_pid = 0;
uint32_t g_cc_classnum = 0;

void push(Kind k, uint8_t x, uint8_t y, bool down)
{
    int next = (g_qhead + 1) % kQueue;
    if (next == g_qtail) return; /* full: drop, never block */
    g_q[g_qhead] = Event{k, x, y, down};
    g_qhead      = next;
}

/* USB-MIDI event packets, 4 bytes each: [cable|CIN, status, data1, data2]. */
void on_rx(uint8_t* buf, size_t len, void*)
{
    for (size_t i = 0; i + 3 < len; i += 4)
    {
        const uint8_t cin = buf[i] & 0x0F;
        const uint8_t st  = buf[i + 1] & 0xF0;
        const uint8_t d1  = buf[i + 2];
        const uint8_t d2  = buf[i + 3];
        if (cin == 0) continue; /* padding */
        g_rx_count++;
        if (st == 0x90 || st == 0x80)
        {
            const bool    down = (st == 0x90) && d2 > 0;
            const uint8_t y = d1 >> 4, x = d1 & 0x0F;
            if (y > 7) continue;
            if (x < 8)       push(Kind::Grid, x, y, down);
            else if (x == 8) push(Kind::Side, 0, y, down);
        }
        else if (st == 0xB0 && d1 >= 104 && d1 <= 111)
        {
            push(Kind::Top, (uint8_t)(d1 - 104), 0, d2 > 0);
        }
    }
}

void on_class_active(void*)
{
    if (!g_usbh.IsActiveClass(USBH_MIDI_CLASS)) return;
    USBH_MIDI_SetReceiveCallback(&hUsbHostHS, on_rx, nullptr);
    g_ready     = true;
    g_need_init = true;
}

void on_error(void*) { g_failed = true; }

void on_disconnect(void*)
{
    g_ready     = false;
    g_need_init = false;
}

uint16_t out_ep_size()
{
    if (!hUsbHostHS.pActiveClass || !hUsbHostHS.pActiveClass->pData) return 0;
    const MIDI_HandleTypeDef* h = (const MIDI_HandleTypeDef*)hUsbHostHS.pActiveClass->pData;
    return h->OutEpSize;
}

void cell_packet(int i, uint8_t col, uint8_t* p)
{
    if (i < kGrid)
    {
        p[0] = 0x09; p[1] = 0x90; p[2] = (uint8_t)(16 * (i / 8) + (i % 8)); p[3] = col;
    }
    else if (i < kTop0)
    {
        p[0] = 0x09; p[1] = 0x90; p[2] = (uint8_t)(16 * (i - kSide0) + 8); p[3] = col;
    }
    else
    {
        p[0] = 0x0B; p[1] = 0xB0; p[2] = (uint8_t)(104 + (i - kTop0)); p[3] = col;
    }
}

void flush()
{
    uint16_t ep = out_ep_size();
    if (ep < 4) return;
    if (ep > sizeof g_tx) ep = sizeof g_tx;
    const int max_pk = ep / 4;
    int       n      = 0;

    int  sent_idx[16];
    int  sent_n = 0;
    bool init   = g_need_init;
    if (init)
    {
        /* Reset (all LEDs off), then the X-Y layout in case a DAW left the
         * pad in drum-rack mode. */
        const uint8_t pk[8] = {0x0B, 0xB0, 0x00, 0x00, 0x0B, 0xB0, 0x00, 0x01};
        memcpy(g_tx, pk, 8);
        n = 2;
    }
    else
    {
        for (int k = 0; k < kCells && n < max_pk && sent_n < 16; k++)
        {
            const int i = (g_scan + k) % kCells;
            if (g_want[i] == g_sent[i]) continue;
            cell_packet(i, g_want[i], &g_tx[4 * n]);
            sent_idx[sent_n++] = i;
            n++;
        }
        if (n == 0) return;
    }

    if (USBH_MIDI_Transmit(&hUsbHostHS, g_tx, (size_t)(4 * n)) != MIDI_OK)
        return; /* busy or error: try again next poll, nothing is marked sent */

    g_tx_count++;
    if (init)
    {
        g_need_init = false;
        for (int i = 0; i < kCells; i++) g_sent[i] = kOff; /* reset cleared them */
    }
    else
    {
        for (int j = 0; j < sent_n; j++) g_sent[sent_idx[j]] = g_want[sent_idx[j]];
        g_scan = (sent_idx[sent_n - 1] + 1) % kCells;
    }
}

} // namespace

void Init()
{
    for (int i = 0; i < kCells; i++) g_want[i] = g_sent[i] = kOff;
    daisy::USBHostHandle::Config cfg;
    cfg.disconnect_callback   = on_disconnect;
    cfg.class_active_callback = on_class_active;
    cfg.error_callback        = on_error;
    g_usbh.Init(cfg);
    g_usbh.RegisterClass(USBH_MIDI_CLASS);
}

void Poll(uint32_t now)
{
    g_usbh.Process();
    const uint8_t gs = (uint8_t)hUsbHostHS.gState;
    const uint8_t es = (uint8_t)hUsbHostHS.EnumState;
    if ((gs != g_last_gs || es != g_last_es) && g_tr_n < kTrace)
        g_tr[g_tr_n++] = Tr{now, gs, es};
    g_last_es = es;
    if (gs == HOST_CHECK_CLASS && !g_cc_seen)
    {
        g_cc_seen = true;
        memcpy(g_cc_dev, hUsbHostHS.device.Data, sizeof g_cc_dev);
        memcpy(g_cc_cfg, hUsbHostHS.device.CfgDesc_Raw, sizeof g_cc_cfg);
        g_cc_total    = hUsbHostHS.device.CfgDesc.wTotalLength;
        g_cc_nif      = hUsbHostHS.device.CfgDesc.bNumInterfaces;
        g_cc_if0c     = hUsbHostHS.device.CfgDesc.Itf_Desc[0].bInterfaceClass;
        g_cc_if1c     = hUsbHostHS.device.CfgDesc.Itf_Desc[1].bInterfaceClass;
        g_cc_if1s     = hUsbHostHS.device.CfgDesc.Itf_Desc[1].bInterfaceSubClass;
        g_cc_if1ep    = hUsbHostHS.device.CfgDesc.Itf_Desc[1].bNumEndpoints;
        g_cc_vid      = hUsbHostHS.device.DevDesc.idVendor;
        g_cc_pid      = hUsbHostHS.device.DevDesc.idProduct;
        g_cc_classnum = hUsbHostHS.ClassNumber;
    }
    if (gs == HOST_ABORT_STATE)
    {
        if (g_last_gs != HOST_ABORT_STATE)
        {
            if (g_diag.aborts == 0)
            {
                g_diag.state_before_abort = g_last_gs;
                g_diag.enum_at_abort      = (uint8_t)(hUsbHostHS.EnumState + 1);
            }
            if (g_diag.aborts < 0xFFFF) g_diag.aborts++;
        }
    }
    g_last_gs = gs;
    if (hUsbHostHS.device.DevDesc.idVendor == 0x1235) g_diag.vid_ok = true;
    if (g_ready && g_usbh.IsActiveClass(USBH_MIDI_CLASS)) flush();
}

bool Connected() { return g_ready; }

uint8_t Stage()
{
    if (g_ready) return 3;
    if (hUsbHostHS.gState == HOST_ABORT_STATE) g_failed = true;
    if (g_failed) return 2;
    return hUsbHostHS.device.is_connected ? 1 : 0;
}

bool PopEvent(Event* e)
{
    if (g_qtail == g_qhead) return false;
    *e      = g_q[g_qtail];
    g_qtail = (g_qtail + 1) % kQueue;
    return true;
}

void SetGrid(uint8_t x, uint8_t y, uint8_t col)
{
    if (x < 8 && y < 8) g_want[y * 8 + x] = col;
}
void SetSide(uint8_t y, uint8_t col)
{
    if (y < 8) g_want[kSide0 + y] = col;
}
void SetTop(uint8_t x, uint8_t col)
{
    if (x < 8) g_want[kTop0 + x] = col;
}
void ClearAll()
{
    for (int i = 0; i < kCells; i++) g_want[i] = kOff;
}

Diag Diagnostics() { return g_diag; }

int Report(char* b, int cap)
{
    int n = 0;
#define OUT(...) do { if (n < cap) n += snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); } while (0)
    OUT("launchpad host report\n");
    OUT("aborts %u, state before first abort %u, enum at first abort %u, vid_ok %d, ready %d\n",
        (unsigned)g_diag.aborts, (unsigned)g_diag.state_before_abort,
        (unsigned)g_diag.enum_at_abort, (int)g_diag.vid_ok, (int)g_ready);
    OUT("rx %lu tx %lu\n", (unsigned long)g_rx_count, (unsigned long)g_tx_count);
    if (g_cc_seen)
    {
        OUT("at CHECK_CLASS: vid %04x pid %04x classes registered %lu\n",
            g_cc_vid, g_cc_pid, (unsigned long)g_cc_classnum);
        OUT("cfg total %u, interfaces %u, if0 class %u, if1 class %u sub %u eps %u\n",
            g_cc_total, g_cc_nif, g_cc_if0c, g_cc_if1c, g_cc_if1s, g_cc_if1ep);
        OUT("device.Data:");
        for (int i = 0; i < 18; i++) OUT(" %02x", g_cc_dev[i]);
        OUT("\nCfgDesc_Raw:");
        for (int i = 0; i < 64; i++) OUT("%s%02x", (i % 16) ? " " : "\n  ", g_cc_cfg[i]);
        OUT("\n");
    }
    else
        OUT("CHECK_CLASS never reached\n");
    OUT("trace (ms gState enumState):\n");
    for (int i = 0; i < g_tr_n; i++) OUT("  %lu %u %u\n", (unsigned long)g_tr[i].t, g_tr[i].gs, g_tr[i].es);
#undef OUT
    return n < cap ? n : cap;
}

uint32_t RxCount() { return g_rx_count; }
uint32_t TxCount() { return g_tx_count; }

} // namespace lp
