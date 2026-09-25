/* usb_audio -- see usb_audio.h. */
#include "usb_audio.h"

#include <string.h>

#include "stm32h7xx_hal.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"
#include "usbd_ioreq.h"

extern PCD_HandleTypeDef hpcd_USB_OTG_HS; /* libDaisy usbd_conf.c */

/* ---- format ---------------------------------------------------------------- */

#define UAC_RATE        48000u
#define UAC_CH          2u
#define UAC_SUBFRAME    3u                        /* 24-bit                 */
#define UAC_FRAME_BYTES (UAC_CH * UAC_SUBFRAME)   /* 6                      */
#define UAC_NOM_FRAMES  (UAC_RATE / 1000u)        /* 48 per 1 ms frame      */
#define UAC_MAX_FRAMES  (UAC_NOM_FRAMES + 1u)
#define UAC_PKT_MAX     (UAC_MAX_FRAMES * UAC_FRAME_BYTES) /* 294          */

#define EP_OUT      0x01u   /* audio from the computer                      */
#define EP_FB       0x81u   /* feedback for EP_OUT                          */
#define EP_IN       0x82u   /* audio to the computer                        */

#define ITF_AC      0u
#define ITF_OUT     1u
#define ITF_IN      2u

/* Ring sizing (frames). The audio callback moves 128 frames at a time on
 * Mark (Smack too), so the fills swing by a block; the targets sit a block
 * plus ~2.7 ms of slack away from empty. */
#define RING        1024u
#define RING_MASK   (RING - 1u)
#define OUT_PRIME   256u
#define OUT_TARGET  192.0f
#define IN_PRIME    192u
#define IN_TARGET   192.0f

/* ---- descriptors ----------------------------------------------------------- */

#define LO(x) (uint8_t)((x) & 0xFFu)
#define HI(x) (uint8_t)(((x) >> 8) & 0xFFu)

#define AC_CS_LEN   (10u + 12u + 9u + 12u + 9u)                /* 52 */
#define AS_OUT_LEN  (9u + 9u + 7u + 11u + 9u + 7u + 9u)        /* 61 */
#define AS_IN_LEN   (9u + 9u + 7u + 11u + 9u + 7u)             /* 52 */
#define CFG_LEN     (9u + 9u + AC_CS_LEN + AS_OUT_LEN + AS_IN_LEN)

#define FORMAT_I                                                              \
    0x0B, 0x24, 0x02, 0x01, UAC_CH, UAC_SUBFRAME, 24, 0x01,                   \
    LO(UAC_RATE), HI(UAC_RATE), (uint8_t)(UAC_RATE >> 16)

static const uint8_t k_cfg[CFG_LEN] = {
    /* configuration */
    0x09, USB_DESC_TYPE_CONFIGURATION, LO(CFG_LEN), HI(CFG_LEN),
    0x03,               /* interfaces                                 */
    0x01, 0x00,         /* value, string                              */
    0xC0,               /* self-powered (the rack)                    */
    0x32,               /* 100 mA                                     */

    /* interface 0: AudioControl */
    0x09, USB_DESC_TYPE_INTERFACE, ITF_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    /* AC header, 2 streaming interfaces */
    0x0A, 0x24, 0x01, 0x00, 0x01, LO(AC_CS_LEN), HI(AC_CS_LEN), 0x02,
    ITF_OUT, ITF_IN,
    /* IT 1: USB streaming, stereo */
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x01, 0x00, UAC_CH, 0x03, 0x00, 0x00, 0x00,
    /* OT 3: line connector (J9/J10), from IT 1 */
    0x09, 0x24, 0x03, 0x03, 0x03, 0x06, 0x00, 0x01, 0x00,
    /* IT 4: line connector (J1/J2), stereo */
    0x0C, 0x24, 0x02, 0x04, 0x03, 0x06, 0x00, UAC_CH, 0x03, 0x00, 0x00, 0x00,
    /* OT 6: USB streaming, from IT 4 */
    0x09, 0x24, 0x03, 0x06, 0x01, 0x01, 0x00, 0x04, 0x00,

    /* interface 1 alt 0: OUT, zero bandwidth */
    0x09, USB_DESC_TYPE_INTERFACE, ITF_OUT, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* interface 1 alt 1: OUT, data + feedback */
    0x09, USB_DESC_TYPE_INTERFACE, ITF_OUT, 0x01, 0x02, 0x01, 0x02, 0x00, 0x00,
    /* AS general: terminal 1, PCM */
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,
    FORMAT_I,
    /* EP 0x01: isochronous, asynchronous, sync endpoint 0x81 */
    0x09, USB_DESC_TYPE_ENDPOINT, EP_OUT, 0x05, LO(UAC_PKT_MAX), HI(UAC_PKT_MAX),
    0x01, 0x00, EP_FB,
    /* CS endpoint: no controls */
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
    /* EP 0x81: isochronous, usage = feedback, 3 bytes, refresh 2^1 ms */
    0x09, USB_DESC_TYPE_ENDPOINT, EP_FB, 0x11, 0x03, 0x00, 0x01, 0x01, 0x00,

    /* interface 2 alt 0: IN, zero bandwidth */
    0x09, USB_DESC_TYPE_INTERFACE, ITF_IN, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* interface 2 alt 1: IN */
    0x09, USB_DESC_TYPE_INTERFACE, ITF_IN, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    /* AS general: terminal 6, PCM */
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00,
    FORMAT_I,
    /* EP 0x82: isochronous, asynchronous */
    0x09, USB_DESC_TYPE_ENDPOINT, EP_IN, 0x05, LO(UAC_PKT_MAX), HI(UAC_PKT_MAX),
    0x01, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,
};
_Static_assert(sizeof k_cfg == CFG_LEN, "configuration descriptor length");
_Static_assert(CFG_LEN == 183u, "UAC1 2-in/2-out with feedback is 183 bytes");

static uint8_t k_dev[USB_LEN_DEV_DESC] = {
    0x12, USB_DESC_TYPE_DEVICE,
    0x10, 0x01,             /* USB 1.1: full speed only, no qualifier     */
    0x00, 0x00, 0x00,       /* class per interface                        */
    USB_MAX_EP0_SIZE,
    0x83, 0x04,             /* 0x0483 */
    0x30, 0x57,             /* 0x5730: not HostLink's 0x5740, so the Mac
                             * does not reuse the CDC device's entry      */
    0x00, 0x01,             /* bcdDevice 1.00                             */
    USBD_IDX_MFC_STR, USBD_IDX_PRODUCT_STR, USBD_IDX_SERIAL_STR,
    0x01,
};

static uint8_t k_langid[USB_LEN_LANGID_STR_DESC] = {
    USB_LEN_LANGID_STR_DESC, USB_DESC_TYPE_STRING, 0x09, 0x04};

static const char *s_product = "Alchemy Lab";
static uint8_t     s_str[2u + 2u * 48u];

static uint8_t *d_device(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    *len = sizeof k_dev;
    return k_dev;
}
static uint8_t *d_langid(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    *len = sizeof k_langid;
    return k_langid;
}
static uint8_t *d_string(const char *text, uint16_t *len)
{
    USBD_GetString((uint8_t *)(uintptr_t)text, s_str, len);
    return s_str;
}
static uint8_t *d_mfc(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    return d_string("Alchemy Lab", len);
}
static uint8_t *d_product(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    return d_string(s_product, len);
}
static uint8_t *d_serial(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    static char hex[25];
    const uint32_t w[3] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
    for (int i = 0; i < 24; i++)
    {
        const uint32_t nib = (w[i / 8] >> (28 - 4 * (i % 8))) & 0xFu;
        hex[i] = (char)(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    hex[24] = 0;
    return d_string(hex, len);
}
static uint8_t *d_config_str(USBD_SpeedTypeDef s, uint16_t *len)
{
    (void)s;
    return d_string("Audio", len);
}

static USBD_DescriptorsTypeDef s_desc = {
    .GetDeviceDescriptor           = d_device,
    .GetLangIDStrDescriptor        = d_langid,
    .GetManufacturerStrDescriptor  = d_mfc,
    .GetProductStrDescriptor       = d_product,
    .GetSerialStrDescriptor        = d_serial,
    .GetConfigurationStrDescriptor = d_config_str,
    .GetInterfaceStrDescriptor     = d_config_str,
};

/* ---- state ------------------------------------------------------------------ */

static USBD_HandleTypeDef s_dev;
static volatile uint8_t   s_started;

/* Frames as signed 24-bit in int32, L then R. */
static int32_t           s_out_ring[RING][UAC_CH];
static volatile uint32_t s_out_wr, s_out_rd;   /* USB writes, audio reads */
static int32_t           s_in_ring[RING][UAC_CH];
static volatile uint32_t s_in_wr, s_in_rd;     /* audio writes, USB reads */

static volatile uint8_t s_out_on, s_in_on;     /* alt settings            */
static volatile uint8_t s_out_primed, s_in_primed;
static volatile uint8_t s_fb_busy, s_in_busy;

static uint8_t s_out_pkt[UAC_PKT_MAX + 6u];
static uint8_t s_in_pkt[UAC_PKT_MAX];
static uint8_t s_fb_pkt[4];
static uint8_t s_ctl[64];

/* Codec frames counted by the audio callback. Only a diagnostic: it moves
 * in whole blocks (128 frames), far coarser than the real rate mismatch, so
 * the control loops run on ring fill alone. */
static volatile uint32_t s_codec_frames;
static uint32_t          s_rate_frames0, s_rate_sofs0;
static float             s_in_frac;
static float             s_out_avg, s_in_avg;

static volatile UAC_Info s_info;

/* ---- helpers ---------------------------------------------------------------- */

static inline int32_t f2s24(float v)
{
    if (v > 0.99999988f) v = 0.99999988f;
    if (v < -1.0f)       v = -1.0f;
    return (int32_t)(v * 8388608.0f);
}

static inline void put24(uint8_t *p, int32_t s)
{
    p[0] = (uint8_t)s;
    p[1] = (uint8_t)(s >> 8);
    p[2] = (uint8_t)(s >> 16);
}

static inline int32_t get24(const uint8_t *p)
{
    int32_t s = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
    return (s << 8) >> 8;
}

static void out_stream(USBD_HandleTypeDef *pdev, uint8_t on)
{
    if (on && !s_out_on)
    {
        USBD_LL_OpenEP(pdev, EP_OUT, USBD_EP_TYPE_ISOC, UAC_PKT_MAX);
        pdev->ep_out[EP_OUT & 0xFu].is_used = 1u;
        USBD_LL_OpenEP(pdev, EP_FB, USBD_EP_TYPE_ISOC, 3u);
        pdev->ep_in[EP_FB & 0xFu].is_used = 1u;
        USBD_LL_FlushEP(pdev, EP_FB);
        s_out_wr     = s_out_rd;          /* producer side: empty the ring */
        s_out_primed = 0u;
        s_fb_busy    = 0u;
        s_out_avg    = OUT_TARGET;
        s_out_on     = 1u;
        USBD_LL_PrepareReceive(pdev, EP_OUT, s_out_pkt, UAC_PKT_MAX);
    }
    else if (!on && s_out_on)
    {
        s_out_on = 0u;
        USBD_LL_CloseEP(pdev, EP_OUT);
        pdev->ep_out[EP_OUT & 0xFu].is_used = 0u;
        USBD_LL_FlushEP(pdev, EP_FB);
        USBD_LL_CloseEP(pdev, EP_FB);
        pdev->ep_in[EP_FB & 0xFu].is_used = 0u;
    }
}

static void in_stream(USBD_HandleTypeDef *pdev, uint8_t on)
{
    if (on && !s_in_on)
    {
        USBD_LL_OpenEP(pdev, EP_IN, USBD_EP_TYPE_ISOC, UAC_PKT_MAX);
        pdev->ep_in[EP_IN & 0xFu].is_used = 1u;
        USBD_LL_FlushEP(pdev, EP_IN);
        s_in_rd     = s_in_wr;            /* consumer side: empty the ring */
        s_in_primed = 0u;
        s_in_busy   = 0u;
        s_in_frac   = 0.0f;
        s_in_avg    = IN_TARGET;
        s_in_on     = 1u;
    }
    else if (!on && s_in_on)
    {
        s_in_on = 0u;
        USBD_LL_FlushEP(pdev, EP_IN);
        USBD_LL_CloseEP(pdev, EP_IN);
        pdev->ep_in[EP_IN & 0xFu].is_used = 0u;
    }
}

/* ---- class callbacks -------------------------------------------------------- */

static void send_in(USBD_HandleTypeDef *pdev);

static uint8_t c_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)pdev;
    (void)cfgidx;
    return (uint8_t)USBD_OK;
}

static uint8_t c_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    out_stream(pdev, 0u);
    in_stream(pdev, 0u);
    return (uint8_t)USBD_OK;
}

static uint8_t c_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    static uint8_t  alt_reply;
    static uint16_t status_reply;
    const uint8_t   recipient = req->bmRequest & 0x1Fu;

    switch (req->bmRequest & USB_REQ_TYPE_MASK)
    {
    case USB_REQ_TYPE_STANDARD:
        switch (req->bRequest)
        {
        case USB_REQ_GET_STATUS:
            status_reply = 0u;
            USBD_CtlSendData(pdev, (uint8_t *)&status_reply, 2u);
            return (uint8_t)USBD_OK;
        case USB_REQ_GET_INTERFACE:
        {
            const uint8_t itf = LOBYTE(req->wIndex);
            alt_reply = itf == ITF_OUT ? s_out_on : (itf == ITF_IN ? s_in_on : 0u);
            USBD_CtlSendData(pdev, &alt_reply, 1u);
            return (uint8_t)USBD_OK;
        }
        case USB_REQ_SET_INTERFACE:
        {
            const uint8_t itf = LOBYTE(req->wIndex);
            const uint8_t alt = LOBYTE(req->wValue);
            if (pdev->dev_state != USBD_STATE_CONFIGURED || alt > 1u
                || (itf == ITF_AC && alt != 0u) || itf > ITF_IN)
            {
                USBD_CtlError(pdev, req);
                return (uint8_t)USBD_FAIL;
            }
            if (itf == ITF_OUT) out_stream(pdev, alt);
            if (itf == ITF_IN)  in_stream(pdev, alt);
            return (uint8_t)USBD_OK;   /* the core sends the status stage */
        }
        case USB_REQ_CLEAR_FEATURE:
            return (uint8_t)USBD_OK;
        default:
            USBD_CtlError(pdev, req);
            return (uint8_t)USBD_FAIL;
        }

    case USB_REQ_TYPE_CLASS:
        /* No units, no controls. A host may still ask about the sampling
         * frequency (CS 0x01 on an endpoint): answer 48 kHz, accept and
         * ignore anything it sets, so nothing stalls. */
        if (req->bmRequest & 0x80u)
        {
            const uint16_t n = req->wLength < sizeof s_ctl ? req->wLength : sizeof s_ctl;
            memset(s_ctl, 0, sizeof s_ctl);
            if (HIBYTE(req->wValue) == 0x01u && recipient == 0x02u)
            {
                s_ctl[0] = LO(UAC_RATE);
                s_ctl[1] = HI(UAC_RATE);
                s_ctl[2] = (uint8_t)(UAC_RATE >> 16);
            }
            USBD_CtlSendData(pdev, s_ctl, n);
        }
        else if (req->wLength)
        {
            const uint16_t n = req->wLength < sizeof s_ctl ? req->wLength : sizeof s_ctl;
            USBD_CtlPrepareRx(pdev, s_ctl, n);
        }
        else if (recipient == 0x02u)
        {
            USBD_CtlSendStatus(pdev);  /* an interface request gets it from the core */
        }
        return (uint8_t)USBD_OK;

    default:
        USBD_CtlError(pdev, req);
        return (uint8_t)USBD_FAIL;
    }
}

static uint8_t c_ep0_rx_ready(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    return (uint8_t)USBD_OK;
}

static uint8_t c_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    (void)pdev;
    if (epnum == (EP_FB & 0x7Fu)) s_fb_busy = 0u;
    if (epnum == (EP_IN & 0x7Fu))
    {
        s_in_busy = 0u;
        s_info.in_packets++;
        /* Queue the next one now: from here the parity is for the next
         * frame. Waiting for SOF would skip every other frame. */
        if (s_in_on) send_in(pdev);
    }
    return (uint8_t)USBD_OK;
}

static uint8_t c_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    if (epnum != EP_OUT || !s_out_on) return (uint8_t)USBD_OK;

    const uint32_t bytes  = USBD_LL_GetRxDataSize(pdev, epnum);
    const uint32_t frames = bytes / UAC_FRAME_BYTES;
    uint32_t       wr     = s_out_wr;
    const uint32_t rd     = s_out_rd;
    const uint8_t *p      = s_out_pkt;
    for (uint32_t i = 0; i < frames; i++, p += UAC_FRAME_BYTES)
    {
        if (wr - rd >= RING) { s_info.out_overruns++; break; }
        s_out_ring[wr & RING_MASK][0] = get24(p);
        s_out_ring[wr & RING_MASK][1] = get24(p + 3);
        wr++;
    }
    __DMB();
    s_out_wr = wr;
    s_info.out_packets++;
    USBD_LL_PrepareReceive(pdev, EP_OUT, s_out_pkt, UAC_PKT_MAX);
    return (uint8_t)USBD_OK;
}

static void send_feedback(USBD_HandleTypeDef *pdev)
{
    float fb = (float)UAC_NOM_FRAMES;
    if (s_out_primed)
    {
        float corr = (OUT_TARGET - s_out_avg) * 0.002f;
        if (corr > 0.2f)  corr = 0.2f;
        if (corr < -0.2f) corr = -0.2f;
        fb += corr;
    }
    const uint32_t v = (uint32_t)(fb * 16384.0f + 0.5f);   /* 10.14 */
    s_fb_pkt[0] = (uint8_t)v;
    s_fb_pkt[1] = (uint8_t)(v >> 8);
    s_fb_pkt[2] = (uint8_t)(v >> 16);
    s_info.feedback = v;
    s_fb_busy = 1u;
    USBD_LL_Transmit(pdev, EP_FB, s_fb_pkt, 3u);
}

static void send_in(USBD_HandleTypeDef *pdev)
{
    uint32_t       rd   = s_in_rd;
    const uint32_t fill = s_in_wr - rd;

    if (!s_in_primed && fill >= IN_PRIME) s_in_primed = 1u;

    /* Frames this millisecond: 48, plus a proportional share of how far the
     * ring sits from its target (the same loop as the OUT feedback), so a
     * fast codec sends the odd 49 and a slow one the odd 47. */
    float corr = 0.0f;
    if (s_in_primed)
    {
        corr = (s_in_avg - IN_TARGET) * 0.002f;
        if (corr > 0.2f)  corr = 0.2f;
        if (corr < -0.2f) corr = -0.2f;
    }
    s_in_frac += (float)UAC_NOM_FRAMES + corr;
    uint32_t n = (uint32_t)s_in_frac;
    s_in_frac -= (float)n;
    if (n < UAC_NOM_FRAMES - 1u) n = UAC_NOM_FRAMES - 1u;
    if (n > UAC_MAX_FRAMES)      n = UAC_MAX_FRAMES;

    uint8_t *p = s_in_pkt;
    for (uint32_t i = 0; i < n; i++, p += UAC_FRAME_BYTES)
    {
        if (s_in_primed && s_in_wr - rd > 0u)
        {
            put24(p, s_in_ring[rd & RING_MASK][0]);
            put24(p + 3, s_in_ring[rd & RING_MASK][1]);
            rd++;
        }
        else
        {
            if (s_in_primed) { s_info.in_underruns++; s_in_primed = 0u; }
            memset(p, 0, UAC_FRAME_BYTES);
        }
    }
    s_in_rd   = rd;
    s_in_busy = 1u;
    USBD_LL_Transmit(pdev, EP_IN, s_in_pkt, (uint16_t)(n * UAC_FRAME_BYTES));
}

static uint8_t c_sof(USBD_HandleTypeDef *pdev)
{
    const uint32_t sofs = ++s_info.sofs;

    if (sofs == 1u)
    {
        s_rate_frames0 = s_codec_frames;
        s_rate_sofs0   = sofs;
    }
    else if ((sofs & 1023u) == 0u)   /* cumulative: the block steps average out */
        s_info.rate = (float)(s_codec_frames - s_rate_frames0) / (float)(sofs - s_rate_sofs0);

    if (s_out_on)
    {
        s_out_avg += ((float)(s_out_wr - s_out_rd) - s_out_avg) * (1.0f / 256.0f);
        s_info.out_fill = s_out_avg;
        if (!s_fb_busy) send_feedback(pdev);
    }
    if (s_in_on)
    {
        s_in_avg += ((float)(s_in_wr - s_in_rd) - s_in_avg) * (1.0f / 256.0f);
        s_info.in_fill = s_in_avg;
        if (!s_in_busy) send_in(pdev);
    }
    return (uint8_t)USBD_OK;
}

static uint8_t c_iso_in_incomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    (void)pdev;
    /* The HAL aborted and disabled the endpoint. Queue again right away (for
     * the next frame) rather than at the next SOF: the host reads feedback
     * every 2 ms, and a SOF-only retry that is one frame out of step with
     * it would miss every time. */
    s_info.in_incomplete++;
    if (epnum == (EP_FB & 0x7Fu))
    {
        s_fb_busy = 0u;
        if (s_out_on) send_feedback(pdev);
    }
    if (epnum == (EP_IN & 0x7Fu))
    {
        s_in_busy = 0u;
        if (s_in_on) send_in(pdev);
    }
    return (uint8_t)USBD_OK;
}

static uint8_t c_iso_out_incomplete(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    s_info.out_incomplete++;
    if (epnum == EP_OUT && s_out_on)
        USBD_LL_PrepareReceive(pdev, EP_OUT, s_out_pkt, UAC_PKT_MAX);
    return (uint8_t)USBD_OK;
}

static uint8_t *c_cfg(uint16_t *len)
{
    *len = (uint16_t)sizeof k_cfg;
    return (uint8_t *)(uintptr_t)k_cfg;
}

static USBD_ClassTypeDef s_class = {
    .Init                          = c_init,
    .DeInit                        = c_deinit,
    .Setup                         = c_setup,
    .EP0_TxSent                    = NULL,
    .EP0_RxReady                   = c_ep0_rx_ready,
    .DataIn                        = c_data_in,
    .DataOut                       = c_data_out,
    .SOF                           = c_sof,
    .IsoINIncomplete               = c_iso_in_incomplete,
    .IsoOUTIncomplete              = c_iso_out_incomplete,
    .GetHSConfigDescriptor         = c_cfg,
    .GetFSConfigDescriptor         = c_cfg,
    .GetOtherSpeedConfigDescriptor = c_cfg,
    .GetDeviceQualifierDescriptor  = NULL,
};

/* ---- public ----------------------------------------------------------------- */

void UAC_Start(const char *product)
{
    if (s_started) return;
    if (product) s_product = product;

    /* libDaisy's USBD_LL_Init brings up the HS core in FS mode on PB14/15
     * (pins, clock, IRQs) and sizes the FIFOs for CDC. */
    USBD_Init(&s_dev, &s_desc, 1 /* DEVICE_HS */);

    /* FIFOs in 32-bit words, set in order (each offset follows the last):
     * RX holds two 294-byte OUT packets plus setup, TX0 control, TX1 the
     * 3-byte feedback, TX2 two 294-byte IN packets. 592 of 1024 words. */
    HAL_PCDEx_SetRxFiFo(&hpcd_USB_OTG_HS, 0x100);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 1, 0x10);
    HAL_PCDEx_SetTxFiFo(&hpcd_USB_OTG_HS, 2, 0x100);

    /* Start-of-frame drives the IN packets, the feedback and the rate
     * measurement; libDaisy leaves it masked. */
    USB_OTG_HS->GINTMSK |= USB_OTG_GINTMSK_SOFM;

    USBD_RegisterClass(&s_dev, &s_class);
    USBD_Start(&s_dev);
    HAL_PWREx_EnableUSBVoltageDetector();
    s_started = 1u;
}

void UAC_Process(const float *const *in, float **out, size_t frames)
{
    s_codec_frames += (uint32_t)frames;

    /* computer -> J9/J10 */
    if (s_out_on)
    {
        uint32_t       rd = s_out_rd;
        const uint32_t wr = s_out_wr;
        __DMB();
        if (!s_out_primed && wr - rd >= OUT_PRIME) s_out_primed = 1u;
        for (size_t i = 0; i < frames; i++)
        {
            if (s_out_primed && wr - rd > 0u)
            {
                out[0][i] = (float)s_out_ring[rd & RING_MASK][0] * (1.0f / 8388608.0f);
                out[1][i] = (float)s_out_ring[rd & RING_MASK][1] * (1.0f / 8388608.0f);
                rd++;
            }
            else
            {
                if (s_out_primed) { s_info.out_underruns++; s_out_primed = 0u; }
                out[0][i] = out[1][i] = 0.0f;
            }
        }
        s_out_rd = rd;
    }
    else
    {
        for (size_t i = 0; i < frames; i++) out[0][i] = out[1][i] = 0.0f;
    }

    /* J1/J2 -> computer */
    if (s_in_on)
    {
        uint32_t       wr = s_in_wr;
        const uint32_t rd = s_in_rd;
        for (size_t i = 0; i < frames; i++)
        {
            if (wr - rd >= RING) { s_info.in_overruns++; break; }
            s_in_ring[wr & RING_MASK][0] = f2s24(in[0][i]);
            s_in_ring[wr & RING_MASK][1] = f2s24(in[1][i]);
            wr++;
        }
        __DMB();
        s_in_wr = wr;
    }
}

uint8_t UAC_State(void)
{
    if (!s_started) return UAC_OFF;
    if (s_dev.dev_state != USBD_STATE_CONFIGURED) return UAC_WAITING;
    return (s_out_on || s_in_on) ? UAC_STREAMING : UAC_IDLE;
}

UAC_Info UAC_GetInfo(void)
{
    UAC_Info i;
    memcpy(&i, (const void *)&s_info, sizeof i);
    i.state  = UAC_State();
    i.out_on = s_out_on;
    i.in_on  = s_in_on;
    return i;
}
