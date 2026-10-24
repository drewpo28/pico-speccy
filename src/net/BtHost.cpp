// On-chip Bluetooth: discovery, A2DP source, HID host (classic + BLE). See BtHost.h.
#if PICOSPECCY_BT

#include "BtHost.h"
#include "WifiNet.h"
#include "app/Debug.h"
#include "app/Buffer.h"
#include "fs/FileUtils.h"
#include "fatfs/ff.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "btstack.h"

// The 31250 Hz audio tick's tap (pwm_audio.cpp, core1 producer -> core0 consumer).
extern volatile int16_t* pcm_bt_ring;
extern volatile uint32_t pcm_bt_w, pcm_bt_r;
extern uint32_t pcm_bt_mask;
extern uint32_t pcm_bt_drops;
// cybt_shared_bus_driver_speccy.c: corrupt BT ring-index reads from the radio.
extern "C" volatile uint32_t cybt_bad_index_reads, cybt_bad_index_giveups;
// hid_app.cpp: the USB input funnels.
void bt_hid_keyboard(uint8_t modifier, const uint8_t keys[6]);
void bt_hid_mouse(bool bl, bool br, bool bm, int32_t dx, int32_t dy, int32_t wheel);
void bt_hid_gamepad(bool up, bool down, bool left, bool right, uint32_t buttons);

namespace BtHost {
namespace {

#define BT_AUDIO_CFG CONFIG_DIR "/bt_audio.txt"
#define BT_INPUT_CFG CONFIG_DIR "/bt_input.txt"

enum State : uint8_t { S_OFF, S_STARTING, S_UP, S_FAILED };
State   s_state = S_OFF;
LogCb   s_log   = nullptr;
btstack_packet_callback_registration_t s_hci_reg;

void blog(const char* fmt, ...) {
    char line[120];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Debug::log("BT: %s", line);
    if (s_log) s_log(line);
}

uint32_t nowMs() { return to_ms_since_boot(get_absolute_time()); }

// ── Discovery ────────────────────────────────────────────────────────────────
constexpr int MAX_DEV = 24;
Dev       s_dev[MAX_DEV];
int       s_ndev    = 0;
bool      s_busy    = false;
bool      s_inquiry = false;   // classic inquiry still running
bool      s_leScan  = false;
uint32_t  s_until   = 0;       // ms deadline of the LE half

Dev* findDev(const bd_addr_t a) {
    for (int i = 0; i < s_ndev; i++)
        if (memcmp(s_dev[i].addr, a, 6) == 0) return &s_dev[i];
    return nullptr;
}

Dev* addDev(const bd_addr_t a) {
    if (s_ndev >= MAX_DEV) return nullptr;
    Dev* d = &s_dev[s_ndev++];
    memset(d, 0, sizeof(*d));
    memcpy(d->addr, a, 6);
    return d;
}

const char* codKind(uint32_t cod) {
    const uint32_t major = (cod >> 8) & 0x1F;
    if (major == 0x05) {               // peripheral
        switch ((cod >> 6) & 0x03) {
            case 1:  return "keyboard";
            case 2:  return "mouse";
            case 3:  return "kbd+mouse";
            default: return (cod & 0x3C) == 0x04 || (cod & 0x3C) == 0x08 ? "gamepad" : "HID";
        }
    }
    if (major == 0x04) return "audio";
    if (major == 0x02) return "phone";
    if (major == 0x01) return "computer";
    return "";
}

const char* appearanceKind(uint16_t ap) {
    switch (ap) {
        case 0x03C1: return "keyboard";
        case 0x03C2: return "mouse";
        case 0x03C3: case 0x03C4: return "gamepad";
        case 0x03C0: return "HID";
        default: break;
    }
    if ((ap >> 6) == 0x0F) return "HID";
    if ((ap >> 6) == 0x29 || (ap >> 6) == 0x0A) return "audio";
    return "";
}

void onInquiryResult(const uint8_t* ev) {
    bd_addr_t a;
    gap_event_inquiry_result_get_bd_addr(ev, a);
    if (findDev(a)) return;
    Dev* d = addDev(a);
    if (!d) return;
    if (gap_event_inquiry_result_get_name_available(ev)) {
        int n = gap_event_inquiry_result_get_name_len(ev);
        if (n > (int)sizeof(d->name) - 1) n = sizeof(d->name) - 1;
        memcpy(d->name, gap_event_inquiry_result_get_name(ev), n);
        d->name[n] = 0;
    }
    d->cod  = gap_event_inquiry_result_get_class_of_device(ev);
    d->rssi = gap_event_inquiry_result_get_rssi_available(ev) ? gap_event_inquiry_result_get_rssi(ev) : 0;
    blog("BR %s %ddBm %s %s", bd_addr_to_str(a), d->rssi, codKind(d->cod),
         d->name[0] ? d->name : "(no name)");
}

void onAdvertising(const uint8_t* ev) {
    bd_addr_t a;
    gap_event_advertising_report_get_address(ev, a);
    const uint8_t len = gap_event_advertising_report_get_data_length(ev);
    const uint8_t* data = gap_event_advertising_report_get_data(ev);
    char name[32] = "";
    uint16_t appearance = 0;
    ad_context_t it;
    for (ad_iterator_init(&it, len, data); ad_iterator_has_more(&it); ad_iterator_next(&it)) {
        const uint8_t t = ad_iterator_get_data_type(&it);
        const uint8_t n = ad_iterator_get_data_len(&it);
        const uint8_t* p = ad_iterator_get_data(&it);
        if ((t == BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME ||
             t == BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME) && !name[0]) {
            const int m = n < sizeof(name) - 1 ? n : sizeof(name) - 1;
            memcpy(name, p, m); name[m] = 0;
        } else if (t == BLUETOOTH_DATA_TYPE_APPEARANCE && n >= 2) {
            appearance = little_endian_read_16(p, 0);
        }
    }
    const bool hid = ad_data_contains_uuid16(len, data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE) ||
                     (appearance >> 6) == 0x0F;
    // The scan response often carries the name the advertisement did not: fill
    // it into a device already listed.
    if (Dev* old = findDev(a)) {
        if (!old->name[0] && name[0]) { strcpy(old->name, name); blog("LE %s is %s", bd_addr_to_str(a), name); }
        old->hid |= hid;
        return;
    }
    // An unnamed beacon with no appearance is noise in this list (phones and
    // trackers advertise dozens); the scan response carries the name, so a device
    // seen first without one gets a second chance when its response arrives.
    if (!name[0] && !appearance && !hid) return;
    Dev* d = addDev(a);
    if (!d) return;
    d->le = true;
    d->hid = hid;
    d->addrType = gap_event_advertising_report_get_address_type(ev);
    d->rssi = gap_event_advertising_report_get_rssi(ev);
    strcpy(d->name, name);
    blog("LE %s %ddBm %s %s", bd_addr_to_str(a), d->rssi,
         appearance ? appearanceKind(appearance) : (hid ? "HID" : ""), name[0] ? name : "(no name)");
}

void finish(const char* why) {
    if (!s_busy) return;
    if (s_inquiry) { gap_inquiry_stop(); s_inquiry = false; }
    if (s_leScan) { gap_stop_scan(); s_leScan = false; }
    s_busy = false;
    blog("%s: %d device(s)", why, s_ndev);
}

// ── A2DP source ──────────────────────────────────────────────────────────────
// What we offer: SBC 44.1/48 kHz, any channel mode / blocks / subbands /
// allocation, bitpool 2..53 (the "high quality" ceiling of the A2DP spec).
uint8_t s_sbcCaps[] = {
    (AVDTP_SBC_44100 << 4) | (AVDTP_SBC_48000 << 4) | AVDTP_SBC_STEREO | AVDTP_SBC_JOINT_STEREO,
    0xFF,
    2, 53,
};
uint8_t s_sbcConfig[4];
uint8_t s_sdpA2dp[150];

AudioState s_audio = A_IDLE;
char       s_audioName[32] = "";
bd_addr_t  s_audioAddr;
uint16_t   s_cid  = 0;
uint8_t    s_seid = 0;        // local stream endpoint

constexpr int SBC_STORAGE = 1030;
constexpr int RING_FRAMES = 2048;   // 65 ms of 31250 Hz stereo

// The heavy, connection-only state: allocated when a stream starts.
struct Stream {
    btstack_sbc_encoder_bluedroid_t enc;
    const btstack_sbc_encoder_t*    encI;
    uint8_t  storage[SBC_STORAGE];  // [0] = SBC media header (frame count)
    uint16_t stored;                // bytes after the header
    uint16_t maxPayload;
    bool     readyToSend;
    uint32_t rtpTs;
    // Resampler 31250 -> fs, linear, Q16 position between prev and cur.
    uint32_t step;                  // 31250/fs in Q16 (< 1.0)
    uint32_t frac;
    int16_t  prevL, prevR, curL, curR;
    bool     primed;
    int16_t  pcm[256 * 2];          // one SBC frame of input (blocks*subbands <= 128)
    int      pcmN;
    // Stats.
    uint32_t pkts, frames, encUs, statT;
};
Stream*  s_st = nullptr;
struct SbcCfg {
    int fs = 44100, blocks = 16, subbands = 8, bitpool = 53;
    btstack_sbc_allocation_method_t alloc = (btstack_sbc_allocation_method_t)SBC_LOUDNESS;
    btstack_sbc_channel_mode_t cm = SBC_CHANNEL_MODE_JOINT_STEREO;
} s_cfg;
int      s_fs = 0;
int16_t* s_ring = nullptr;

void ringOff() {
    pcm_bt_ring = nullptr;
    __dmb();
    // The 31250 Hz tick on core1 may have loaded the pointer just before it was
    // cleared and still be storing into it: let two ticks (64 us) pass before
    // the block goes back to the heap.
    if (s_ring) { busy_wait_us(100); Buffer::pfree(s_ring); s_ring = nullptr; }
}

void streamFree() {
    ringOff();
    if (s_st) { Buffer::pfree(s_st); s_st = nullptr; }
}

void saveRemembered() {
    if (!FileUtils::fsMount) return;
    FIL f;
    if (f_open(&f, BT_AUDIO_CFG, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    char line[64];
    const int n = snprintf(line, sizeof(line), "%s\t%s\n", bd_addr_to_str(s_audioAddr), s_audioName);
    UINT w; f_write(&f, line, n, &w);
    f_close(&f);
}

void sendPacket() {
    Stream* st = s_st;
    if (!st || !st->stored) { if (st) st->readyToSend = false; return; }
    const int fl = st->encI->sbc_buffer_length(&st->enc);
    const uint8_t frames = (uint8_t)(st->stored / fl);
    st->storage[0] = frames;
    a2dp_source_stream_send_media_payload_rtp(s_cid, s_seid, 0, st->rtpTs, st->storage, st->stored + 1);
    st->rtpTs += frames * st->encI->num_audio_frames(&st->enc);
    st->stored = 0;
    st->readyToSend = false;
    st->pkts++;
}

// Take what core1 has produced, resample, encode into the packet buffer, and ask
// for a send slot when the next SBC frame would not fit.
void audioWork() {
    Stream* st = s_st;
    if (!st || s_audio != A_STREAMING || !s_ring) return;
    const int fl  = st->encI->sbc_buffer_length(&st->enc);
    const int nin = st->encI->num_audio_frames(&st->enc);
    uint32_t r = pcm_bt_r;
    const uint32_t w = pcm_bt_w;
    __dmb();
    // Latency cap: a stall (menu flush, SD load) must not leave the stream
    // permanently behind — keep the newest ~30 ms.
    if (w - r > (uint32_t)(RING_FRAMES * 3 / 4)) r = w - 1024;
    const uint32_t t0 = time_us_32();
    while (!st->readyToSend) {
        // Fill one SBC frame of output samples.
        while (st->pcmN < nin) {
            if (st->frac >= 0x10000u || !st->primed) {
                if (r == w) goto out;
                const uint32_t i = (r & pcm_bt_mask) << 1;
                st->prevL = st->curL; st->prevR = st->curR;
                st->curL = s_ring[i]; st->curR = s_ring[i + 1];
                r++;
                if (!st->primed) { st->primed = true; st->prevL = st->curL; st->prevR = st->curR; st->frac = 0; continue; }
                st->frac -= 0x10000u;
                continue;
            }
            const int32_t f = (int32_t)st->frac;
            st->pcm[st->pcmN * 2]     = (int16_t)(st->prevL + (((st->curL - st->prevL) * f) >> 16));
            st->pcm[st->pcmN * 2 + 1] = (int16_t)(st->prevR + (((st->curR - st->prevR) * f) >> 16));
            st->pcmN++;
            st->frac += st->step;
        }
        if (st->stored + fl > st->maxPayload) {
            st->readyToSend = true;
            break;
        }
        st->encI->encode_signed_16(&st->enc, st->pcm, &st->storage[1 + st->stored]);
        st->stored += fl;
        st->pcmN = 0;
        st->frames++;
        if (st->stored + fl > st->maxPayload) { st->readyToSend = true; break; }
    }
out:
    pcm_bt_r = r;
    st->encUs += time_us_32() - t0;
    if (st->readyToSend) a2dp_source_stream_endpoint_request_can_send_now(s_cid, s_seid);

    const uint32_t now = nowMs();
    if (now - st->statT >= 5000) {
        Debug::log("BT audio: %u pkt %u sbc/5s, enc %u ms/5s, drops %u, ring %u, bad idx %u/%u",
                   (unsigned)st->pkts, (unsigned)st->frames, (unsigned)(st->encUs / 1000),
                   (unsigned)pcm_bt_drops, (unsigned)(pcm_bt_w - pcm_bt_r),
                   (unsigned)cybt_bad_index_reads, (unsigned)cybt_bad_index_giveups);
        st->pkts = st->frames = st->encUs = 0;
        st->statT = now;
    }
}

bool streamStart() {
    if (!s_st) {
        s_st = (Stream*)Buffer::palloc(sizeof(Stream), Buffer::NEED_POINTER | Buffer::HOT_SRAM);
        if (!s_st) { blog("no memory for the SBC encoder (%u B)", (unsigned)sizeof(Stream)); return false; }
    }
    if (!s_ring) {
        s_ring = (int16_t*)Buffer::palloc(RING_FRAMES * 4, Buffer::NEED_POINTER | Buffer::HOT_SRAM);
        if (!s_ring) { blog("no memory for the audio ring"); streamFree(); return false; }
    }
    Stream* st = s_st;
    memset(st, 0, sizeof(*st));
    st->encI = btstack_sbc_encoder_bluedroid_init_instance(&st->enc);
    // The configuration as negotiated (A2DP_SUBEVENT_..._SBC_CONFIGURATION).
    s_fs = s_cfg.fs;
    const int blocks = s_cfg.blocks, subbands = s_cfg.subbands, bitpool = s_cfg.bitpool;
    const btstack_sbc_allocation_method_t alloc = s_cfg.alloc;
    const btstack_sbc_channel_mode_t cm = s_cfg.cm;
    st->encI->configure(&st->enc, SBC_MODE_STANDARD, blocks, subbands, alloc, s_fs, bitpool, cm);
    // a2dp_max_media_payload_size() is an int and may be <= 0 if the channel is
    // not what we think it is: never let it wrap into a huge uint16 limit.
    int mp = a2dp_max_media_payload_size(s_cid, s_seid);
    if (mp > SBC_STORAGE) mp = SBC_STORAGE;
    // ...and to OUR outgoing buffer. avdtp_source_stream_send_media_payload_rtp()
    // checks the packet against the SINK's L2CAP MTU only, then memcpy()s it into
    // BTstack's outgoing HCI buffer, which is sized by HCI_ACL_PAYLOAD_SIZE: a sink
    // with a big MTU let a 1030-byte packet overrun it and trash the variables
    // behind it (hw 2026-10-24: the next HCI write then read from 0x20081fe8 and
    // bus-faulted at the end of SRAM). 12 = the RTP media header.
    const int local = (int)l2cap_max_mtu() - 12;
    if (mp > local) mp = local;
    if (mp < 2 + st->encI->sbc_buffer_length(&st->enc)) {
        blog("media payload size %d too small - not streaming", mp);
        return false;
    }
    st->maxPayload = (uint16_t)(mp - 1);
    st->step = (uint32_t)(((uint64_t)31250 << 16) / (uint32_t)s_fs);
    st->statT = nowMs();
    blog("SBC %d Hz, %d blocks, %d subbands, bitpool %d, payload %u", s_fs, blocks, subbands,
         bitpool, (unsigned)st->maxPayload);

    pcm_bt_mask = RING_FRAMES - 1;
    pcm_bt_r = pcm_bt_w = 0;
    pcm_bt_drops = 0;
    __dmb();
    pcm_bt_ring = s_ring;
    return true;
}

void a2dpHandler(uint8_t type, uint16_t, uint8_t* p, uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(p) != HCI_EVENT_A2DP_META) return;
    switch (hci_event_a2dp_meta_get_subevent_code(p)) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            const uint8_t status = a2dp_subevent_signaling_connection_established_get_status(p);
            if (status != ERROR_CODE_SUCCESS) {
                blog("connection failed, status 0x%02x", status);
                s_audio = A_FAILED; s_cid = 0;
                break;
            }
            s_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(p);
            blog("signaling up, negotiating...");
            break;
        }
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            s_cfg.fs       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(p);
            s_cfg.blocks   = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(p);
            s_cfg.subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(p);
            s_cfg.bitpool  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(p);
            s_cfg.alloc    = (btstack_sbc_allocation_method_t)
                (a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(p) - 1);
            switch ((avdtp_channel_mode_t)a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(p)) {
                case AVDTP_CHANNEL_MODE_STEREO:       s_cfg.cm = SBC_CHANNEL_MODE_STEREO; break;
                case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: s_cfg.cm = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
                case AVDTP_CHANNEL_MODE_MONO:         s_cfg.cm = SBC_CHANNEL_MODE_MONO; break;
                default:                              s_cfg.cm = SBC_CHANNEL_MODE_JOINT_STEREO; break;
            }
            blog("sink picked SBC %u Hz, bitpool %u..%u",
                 (unsigned)a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(p),
                 (unsigned)a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(p),
                 (unsigned)a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(p));
            break;
        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            const uint8_t status = a2dp_subevent_stream_established_get_status(p);
            if (status != ERROR_CODE_SUCCESS) {
                blog("stream failed, status 0x%02x", status);
                s_audio = A_FAILED;
                break;
            }
            s_seid = a2dp_subevent_stream_established_get_local_seid(p);
            a2dp_source_start_stream(s_cid, s_seid);
            break;
        }
        case A2DP_SUBEVENT_STREAM_STARTED:
            if (!streamStart()) {
                a2dp_source_disconnect(s_cid);
                s_audio = A_FAILED;
                break;
            }
            s_audio = A_STREAMING;
            saveRemembered();
            blog("streaming to %s", s_audioName[0] ? s_audioName : bd_addr_to_str(s_audioAddr));
            break;
        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            sendPacket();
            break;
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            blog("stream suspended by the sink");
            ringOff();
            if (s_audio == A_STREAMING) s_audio = A_CONNECTING;
            break;
        case A2DP_SUBEVENT_STREAM_RELEASED:
            blog("stream released");
            streamFree();
            if (s_audio == A_STREAMING || s_audio == A_CONNECTING) s_audio = A_IDLE;
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            blog("disconnected");
            streamFree();
            s_cid = 0;
            if (s_audio != A_FAILED) s_audio = A_IDLE;
            break;
        default:
            break;
    }
}

// ── Input: HID host ─────────────────────────────────────────────────────────
InputState s_in = I_IDLE;
char       s_inName[32] = "";
bd_addr_t  s_inAddr;
uint8_t    s_inAddrType = 0;        // LE address type
bool       s_inLe = false;
bool       s_inUser = false;        // the user asked for this connection (not a reconnect)
uint16_t   s_hidCid  = 0;           // classic hid_host
hid_protocol_mode_t s_hidMode = HID_PROTOCOL_MODE_REPORT;
bool       s_hidDescOk = false;
hci_con_handle_t s_leHandle = HCI_CON_HANDLE_INVALID;
uint16_t   s_hidsCid = 0;           // BLE hids_host
btstack_packet_callback_registration_t s_sm_reg;
// Descriptor storage the two hosts keep the devices' HID descriptors in.
uint8_t    s_hidDescStore[512];
uint8_t    s_hidsDescStore[512];

// Absolute X/Y (and the hat) ranges of the connected device, from its descriptor:
// a pad's stick is 0..255 on one device and 0..1023 or -32768..32767 on the next.
struct AxisRange { uint16_t rid; uint16_t usage; int32_t lo, hi; };
AxisRange s_axes[8];
int       s_naxes = 0;

void axesScan(const uint8_t* desc, uint16_t len) {
    s_naxes = 0;
    if (!desc || !len) return;
    btstack_hid_usage_iterator_t it;
    btstack_hid_usage_item_t item;
    btstack_hid_usage_iterator_init(&it, desc, len, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&it) && s_naxes < 8) {
        btstack_hid_usage_iterator_get_item(&it, &item);
        if (item.usage_page != 0x01) continue;
        if (item.usage != 0x30 && item.usage != 0x31 && item.usage != 0x39) continue;
        s_axes[s_naxes++] = { item.report_id, item.usage,
                              it.global_logical_minimum, it.global_logical_maximum };
    }
}

bool axisRange(uint16_t rid, uint16_t usage, int32_t& lo, int32_t& hi) {
    for (int i = 0; i < s_naxes; i++)
        if (s_axes[i].usage == usage && (s_axes[i].rid == rid || s_axes[i].rid == HID_REPORT_ID_UNDEFINED)) {
            lo = s_axes[i].lo; hi = s_axes[i].hi;
            return hi > lo;
        }
    return false;
}

void inputRelease() {
    static const uint8_t none[6] = {0};
    bt_hid_keyboard(0, none);
    bt_hid_mouse(false, false, false, 0, 0, 0);
    bt_hid_gamepad(false, false, false, false, 0);
}

// One input report, through the device's descriptor (or the boot one).
void inputReport(const uint8_t* desc, uint16_t dlen, const uint8_t* rep, uint16_t rlen) {
    if (!desc || !dlen || !rlen) return;
    btstack_hid_parser_t ps;
    btstack_hid_parser_init(&ps, desc, dlen, HID_REPORT_TYPE_INPUT, rep, rlen);
    uint8_t mod = 0, keys[6] = {0};
    int nk = 0;
    bool kbd = false, rel = false, absx = false, absy = false, hat = false, btns = false;
    int32_t dx = 0, dy = 0, wheel = 0, ax = 0, ay = 0, hv = 0;
    uint32_t buttons = 0;
    // With report IDs the first byte is the ID; axisRange() ignores it for a
    // descriptor without them (their ranges are stored as UNDEFINED).
    const uint16_t rid = rep[0];
    while (btstack_hid_parser_has_more(&ps)) {
        uint16_t page, usage; int32_t value;
        btstack_hid_parser_get_field(&ps, &page, &usage, &value);
        const bool relative = (ps.descriptor_usage_item.descriptor_item.item_value & 4) != 0;
        if (page == 0x07) {
            kbd = true;
            if (usage >= 0xE0 && usage <= 0xE7) { if (value) mod |= (uint8_t)(1u << (usage - 0xE0)); }
            else if (usage >= 0x04 && usage < 0xE0 && value && nk < 6) keys[nk++] = (uint8_t)usage;
        } else if (page == 0x09) {
            btns = true;
            if (usage >= 1 && usage <= 32 && value) buttons |= 1u << (usage - 1);
        } else if (page == 0x01) {
            switch (usage) {
                case 0x30: if (relative) { dx = value; rel = true; } else { ax = value; absx = true; } break;
                case 0x31: if (relative) { dy = value; rel = true; } else { ay = value; absy = true; } break;
                case 0x38: wheel = value; break;
                case 0x39: hv = value; hat = true; break;
                default: break;
            }
        }
    }
    if (kbd) bt_hid_keyboard(mod, keys);
    if (rel) {
        bt_hid_mouse(buttons & 1, buttons & 2, buttons & 4, dx, dy, wheel);
    } else if (absx || absy || hat || (btns && !kbd)) {
        bool up = false, down = false, left = false, right = false;
        int32_t lo, hi;
        if (absx && axisRange(rid, 0x30, lo, hi)) {
            const int32_t q = (hi - lo) / 4;
            left = ax < lo + q; right = ax > hi - q;
        }
        if (absy && axisRange(rid, 0x31, lo, hi)) {
            const int32_t q = (hi - lo) / 4;
            up = ay < lo + q; down = ay > hi - q;
        }
        if (hat) {
            int32_t hlo = 0, hhi = 7;
            axisRange(rid, 0x39, hlo, hhi);
            const int32_t d = hv - hlo;       // 0 = N, clockwise; out of range = centred
            if (d >= 0 && d <= 7) {
                up    |= d == 7 || d == 0 || d == 1;
                right |= d >= 1 && d <= 3;
                down  |= d >= 3 && d <= 5;
                left  |= d >= 5 && d <= 7;
            }
        }
        bt_hid_gamepad(up, down, left, right, buttons);
    }
}

void saveInput() {
    if (!FileUtils::fsMount) return;
    FIL f;
    if (f_open(&f, BT_INPUT_CFG, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    char line[80];
    const int n = snprintf(line, sizeof(line), "%s\t%c\t%u\t%s\n", bd_addr_to_str(s_inAddr),
                           s_inLe ? 'L' : 'C', (unsigned)s_inAddrType, s_inName);
    UINT w; f_write(&f, line, n, &w);
    f_close(&f);
}

// "addr\tC|L\taddrType\tname"
bool loadInput(bd_addr_t a, bool& le, uint8_t& type, char* name, int cap) {
    if (!FileUtils::fsMount) return false;
    FIL f;
    if (f_open(&f, BT_INPUT_CFG, FA_READ) != FR_OK) return false;
    char line[96] = {0};
    UINT n = 0;
    f_read(&f, line, sizeof(line) - 1, &n);
    f_close(&f);
    line[n] = 0;
    char* f1 = strchr(line, '\t');        if (!f1) return false; *f1++ = 0;
    char* f2 = strchr(f1, '\t');          if (!f2) return false; *f2++ = 0;
    char* f3 = strchr(f2, '\t');          if (!f3) return false; *f3++ = 0;
    char* nl = strpbrk(f3, "\r\n");       if (nl) *nl = 0;
    if (!sscanf_bd_addr(line, a)) return false;
    le = f1[0] == 'L';
    type = (uint8_t)(f2[0] - '0');
    snprintf(name, cap, "%s", f3);
    return true;
}

// The remembered LE device reconnects when it advertises: keep the controller
// listening for it (BTstack's auto-connect = whitelist + background connect).
void leAutoConnect(bool on) {
    bd_addr_t a; bool le; uint8_t t; char nm[32];
    if (!loadInput(a, le, t, nm, sizeof(nm)) || !le) return;
    if (on) gap_auto_connection_start((bd_addr_type_t)t, a);
    else    gap_auto_connection_stop((bd_addr_type_t)t, a);
}

void inputReady() {
    s_in = I_READY;
    saveInput();
    blog("%s ready (%s)", s_inName, s_inLe ? "BLE" : "classic");
}

void inputClosed(const char* why) {
    inputRelease();
    s_hidCid = 0; s_hidsCid = 0; s_leHandle = HCI_CON_HANDLE_INVALID;
    s_hidDescOk = false;
    if (s_in != I_FAILED) s_in = I_IDLE;
    blog("%s: %s", s_inName[0] ? s_inName : "input", why);
    if (s_inLe) leAutoConnect(true);       // wait for it to come back
}

void hidHandler(uint8_t type, uint16_t, uint8_t* p, uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(p) != HCI_EVENT_HID_META) return;
    bd_addr_t a;
    switch (hci_event_hid_meta_get_subevent_code(p)) {
        case HID_SUBEVENT_INCOMING_CONNECTION: {
            // A classic device pages us: the remembered one waking up, or the one
            // being paired. Anything else is declined.
            hid_subevent_incoming_connection_get_address(p, a);
            bd_addr_t ra; bool le; uint8_t t; char nm[32];
            const bool known = (loadInput(ra, le, t, nm, sizeof(nm)) && !le && bd_addr_cmp(ra, a) == 0) ||
                               (s_in == I_CONNECTING && !s_inLe && bd_addr_cmp(s_inAddr, a) == 0);
            const uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(p);
            if (!known || s_hidCid) { hid_host_decline_connection(cid); break; }
            if (s_in != I_CONNECTING) { memcpy(s_inAddr, a, 6); snprintf(s_inName, sizeof(s_inName), "%s", nm); s_inLe = false; }
            s_hidCid = cid;
            s_in = I_CONNECTING;
            hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT);
            blog("%s is connecting", s_inName);
            break;
        }
        case HID_SUBEVENT_CONNECTION_OPENED: {
            const uint8_t st = hid_subevent_connection_opened_get_status(p);
            if (st != ERROR_CODE_SUCCESS) {
                blog("connection failed, status 0x%02x", st);
                s_in = I_FAILED; s_hidCid = 0;
                break;
            }
            s_hidCid = hid_subevent_connection_opened_get_hid_cid(p);
            s_hidMode = HID_PROTOCOL_MODE_REPORT;
            blog("connected, reading its HID descriptor...");
            break;
        }
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            s_hidDescOk = hid_subevent_descriptor_available_get_status(p) == ERROR_CODE_SUCCESS;
            if (s_hidDescOk) axesScan(hid_descriptor_storage_get_descriptor_data(s_hidCid),
                                      hid_descriptor_storage_get_descriptor_len(s_hidCid));
            blog(s_hidDescOk ? "descriptor %u bytes" : "no descriptor (boot protocol)",
                 (unsigned)hid_descriptor_storage_get_descriptor_len(s_hidCid));
            inputReady();
            break;
        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            if (hid_subevent_set_protocol_response_get_handshake_status(p) == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL)
                s_hidMode = (hid_protocol_mode_t)hid_subevent_set_protocol_response_get_protocol_mode(p);
            break;
        case HID_SUBEVENT_REPORT: {
            const uint8_t* r = hid_subevent_report_get_report(p);
            uint16_t n = hid_subevent_report_get_report_len(p);
            if (n < 1 || r[0] != 0xA1) break;        // DATA | Input
            r++; n--;
            if (s_hidMode == HID_PROTOCOL_MODE_BOOT || !s_hidDescOk)
                inputReport(btstack_hid_get_boot_descriptor_data(), btstack_hid_get_boot_descriptor_len(), r, n);
            else
                inputReport(hid_descriptor_storage_get_descriptor_data(s_hidCid),
                            hid_descriptor_storage_get_descriptor_len(s_hidCid), r, n);
            break;
        }
        case HID_SUBEVENT_CONNECTION_CLOSED:
            if (hid_subevent_connection_closed_get_hid_cid(p) == s_hidCid) inputClosed("disconnected");
            break;
        default: break;
    }
}

void hidsHandler(uint8_t type, uint16_t, uint8_t* p, uint16_t) {
    if (type != HCI_EVENT_PACKET || hci_event_packet_get_type(p) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(p)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            if (gattservice_subevent_hid_service_connected_get_status(p) != ERROR_CODE_SUCCESS) {
                blog("HID service not found (0x%02x)", gattservice_subevent_hid_service_connected_get_status(p));
                s_in = I_FAILED;
                gap_disconnect(s_leHandle);
                break;
            }
            axesScan(hids_host_descriptor_storage_get_descriptor_data(s_hidsCid, 0),
                     hids_host_descriptor_storage_get_descriptor_len(s_hidsCid, 0));
            inputReady();
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t si = gattservice_subevent_hid_report_get_service_index(p);
            inputReport(hids_host_descriptor_storage_get_descriptor_data(s_hidsCid, si),
                        hids_host_descriptor_storage_get_descriptor_len(s_hidsCid, si),
                        gattservice_subevent_hid_report_get_report(p),
                        gattservice_subevent_hid_report_get_report_len(p));
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            s_hidsCid = 0;
            break;
        default: break;
    }
}

void smHandler(uint8_t type, uint16_t, uint8_t* p, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    bool go = false;
    switch (hci_event_packet_get_type(p)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            sm_just_works_confirm(sm_event_just_works_request_get_handle(p));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(p));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            blog("type %06u then Enter on the keyboard", (unsigned)sm_event_passkey_display_number_get_passkey(p));
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_status(p) == ERROR_CODE_SUCCESS) { blog("paired"); go = true; }
            else { blog("pairing failed (0x%02x)", sm_event_pairing_complete_get_status(p)); s_in = I_FAILED; gap_disconnect(s_leHandle); }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (sm_event_reencryption_complete_get_status(p) == ERROR_CODE_SUCCESS) go = true;
            else { blog("re-encryption failed (0x%02x) - pair it again", sm_event_reencryption_complete_get_status(p));
                   s_in = I_FAILED; gap_disconnect(s_leHandle); }
            break;
        default: break;
    }
    if (go && s_leHandle != HCI_CON_HANDLE_INVALID && !s_hidsCid) {
        blog("reading the HID service...");
        hids_host_connect(s_leHandle, hidsHandler, HID_PROTOCOL_MODE_REPORT, &s_hidsCid);
    }
}

// LE link events (from packetHandler).
void leConnected(const uint8_t* p) {
    if (gap_subevent_le_connection_complete_get_status(p) != ERROR_CODE_SUCCESS) return;
    bd_addr_t a;
    gap_subevent_le_connection_complete_get_peer_address(p, a);
    bd_addr_t ra; bool le; uint8_t t; char nm[32];
    const bool pairing = s_in == I_CONNECTING && s_inLe && bd_addr_cmp(a, s_inAddr) == 0;
    const bool known   = loadInput(ra, le, t, nm, sizeof(nm)) && le && bd_addr_cmp(ra, a) == 0;
    if (!pairing && !known) return;        // not ours (the audio side never uses LE)
    if (!pairing) { memcpy(s_inAddr, a, 6); s_inAddrType = t; snprintf(s_inName, sizeof(s_inName), "%s", nm); s_inLe = true; }
    s_leHandle = gap_subevent_le_connection_complete_get_connection_handle(p);
    s_in = I_CONNECTING;
    leAutoConnect(false);
    blog("%s connected, securing the link...", s_inName);
    sm_request_pairing(s_leHandle);         // pairs, or re-encrypts a bonded device
}

void leDisconnected(hci_con_handle_t h) {
    if (h != s_leHandle) return;
    inputClosed("disconnected");
}

void servicesInit() {
    l2cap_init();
    sdp_init();
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dpHandler);
    avdtp_stream_endpoint_t* ep = a2dp_source_create_stream_endpoint(
        AVDTP_AUDIO, AVDTP_CODEC_SBC, s_sbcCaps, sizeof(s_sbcCaps), s_sbcConfig, sizeof(s_sbcConfig));
    if (ep) {
        avdtp_set_preferred_sampling_frequency(ep, 44100);   // less encoding than 48k
        s_seid = avdtp_local_seid(ep);
    } else {
        blog("no A2DP stream endpoint (MAX_NR_AVDTP_STREAM_ENDPOINTS)");
    }
    memset(s_sdpA2dp, 0, sizeof(s_sdpA2dp));
    a2dp_source_create_sdp_record(s_sdpA2dp, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(s_sdpA2dp);
    // Input: classic HID host + BLE HID-over-GATT host.
    hid_host_init(s_hidDescStore, sizeof(s_hidDescStore));
    hid_host_register_packet_handler(&hidHandler);
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    s_sm_reg.callback = &smHandler;
    sm_add_event_handler(&s_sm_reg);
    gatt_client_init();
    hids_host_init(s_hidsDescStore, sizeof(s_hidsDescStore));

    gap_set_local_name("Pico-Speccy");
    gap_set_class_of_device(0x200408);       // audio service, A/V hifi
    // DisplayOnly: an audio sink (NoInputNoOutput) still pairs "Just Works", a
    // keyboard gets a passkey to type (logged in the pane).
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_ONLY);
    // A paired classic keyboard / pad reconnects by paging us on a keypress.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    gap_connectable_control(1);
}

void packetHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    bd_addr_t a;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING && s_state != S_UP) {
                s_state = S_UP;
                gap_local_bd_addr(a);
                blog("controller up, %s", bd_addr_to_str(a));
                leAutoConnect(true);              // a remembered BLE keyboard/mouse/pad
            }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:          // legacy pairing (pre-2.1 devices)
            hci_event_pin_code_request_get_bd_addr(packet, a);
            if (s_in == I_CONNECTING && bd_addr_cmp(a, s_inAddr) == 0)
                blog("PIN 0000: type 0000 then Enter on the keyboard");
            else
                blog("PIN request -> 0000");
            gap_pin_code_response(a, "0000");
            break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION: // SSP passkey entry (keyboards)
            blog("type %06u then Enter on the keyboard",
                 (unsigned)hci_event_user_passkey_notification_get_numeric_value(packet));
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                leConnected(packet);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            leDisconnected(hci_event_disconnection_complete_get_connection_handle(packet));
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: // SSP "Just Works"
            hci_event_user_confirmation_request_get_bd_addr(packet, a);
            gap_ssp_confirmation_response(a);
            break;
        case GAP_EVENT_INQUIRY_RESULT:  if (s_busy) onInquiryResult(packet); break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            s_inquiry = false;
            if (s_busy && (!s_leScan || (int32_t)(nowMs() - s_until) >= 0)) finish("scan done");
            break;
        case GAP_EVENT_ADVERTISING_REPORT: if (s_busy) onAdvertising(packet); break;
        default: break;
    }
}

// Start the controller (BT firmware download) without waiting. False = no radio.
uint32_t s_powerT0 = 0;
bool powerOnAsync() {
    if (s_state != S_OFF) return s_state != S_FAILED;
    if (!WifiNet::ready() || !WifiNet::busUp()) {
        s_state = S_FAILED;
        blog("radio not available");
        return false;
    }
    s_hci_reg.callback = &packetHandler;
    hci_add_event_handler(&s_hci_reg);
    servicesInit();
    s_state = S_STARTING;
    blog("powering up (BT firmware download)...");
    s_powerT0 = nowMs();
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    hci_power_control(HCI_POWER_ON);
    return true;
}

// Power up once per session, waiting (pumped) for HCI_STATE_WORKING.
bool ensureUp() {
    if (s_state == S_UP) return true;
    if (!powerOnAsync()) return false;
    const uint32_t t0 = s_powerT0;
    while (s_state == S_STARTING && nowMs() - t0 < 8000) {
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(5));
        WifiNet::poll();
    }
    if (s_state != S_UP) {
        s_state = S_FAILED;
        blog("HCI did not come up (state %d) - Bluetooth off for this session",
             (int)hci_get_state());
        return false;
    }
    blog("up in %u ms", (unsigned)(nowMs() - t0));
    return true;
}

}  // namespace

void setLog(LogCb cb) { s_log = cb; }

bool scanBegin(int seconds, bool classicOnly) {
    if (!ensureUp()) return false;
    if (s_busy) finish("restart");
    s_ndev = 0;
    s_busy = true;
    if (seconds < 3) seconds = 3;
    s_until = nowMs() + (uint32_t)seconds * 1000u;
    // Inquiry length is in 1.28 s units; the LE half runs until s_until.
    const int units = (seconds * 100 + 127) / 128;
    s_inquiry = gap_inquiry_start((uint8_t)(units > 48 ? 48 : units)) == 0;
    s_leScan = !classicOnly;
    if (s_leScan) {
        gap_set_scan_params(1, 0x0030, 0x0030, 0);   // active, 30 ms / 30 ms
        gap_start_scan();
    }
    blog("scanning %d s (classic%s%s)...", seconds, s_inquiry ? "" : " FAILED",
         s_leScan ? " + LE" : "");
    return true;
}

bool scanBusy() {
    if (s_busy && !s_inquiry && (int32_t)(nowMs() - s_until) >= 0) finish("scan done");
    return s_busy;
}

void scanStop() { finish("stopped"); }

int found() { return s_ndev; }
int devCount() { return s_ndev; }
const Dev* dev(int i) { return (i >= 0 && i < s_ndev) ? &s_dev[i] : nullptr; }
bool isAudio(const Dev& d) { return !d.le && ((d.cod >> 8) & 0x1F) == 0x04; }
bool isHid(const Dev& d) { return d.le ? d.hid : ((d.cod >> 8) & 0x1F) == 0x05; }

InputState inputState() { return s_in; }
const char* inputName() { return s_inName; }

bool inputConnect(const Dev& d) {
    if (!ensureUp()) return false;
    if (s_busy) finish("stopped");
    if (s_in == I_READY || s_in == I_CONNECTING) inputDisconnect();
    leAutoConnect(false);
    memcpy(s_inAddr, d.addr, 6);
    s_inLe = d.le;
    s_inAddrType = d.addrType;
    snprintf(s_inName, sizeof(s_inName), "%s", d.name[0] ? d.name : bd_addr_to_str(s_inAddr));
    s_in = I_CONNECTING;
    uint8_t rc;
    if (d.le) {
        rc = gap_connect(s_inAddr, (bd_addr_type_t)s_inAddrType);
    } else {
        uint16_t cid = 0;
        rc = hid_host_connect(s_inAddr, HID_PROTOCOL_MODE_REPORT, &cid);
        if (rc == ERROR_CODE_SUCCESS) s_hidCid = cid;
    }
    if (rc != ERROR_CODE_SUCCESS) {
        blog("connect failed to start, status 0x%02x", rc);
        s_in = I_FAILED;
        return false;
    }
    blog("connecting to %s (%s)...", s_inName, d.le ? "BLE" : "classic");
    return true;
}

void inputDisconnect() {
    if (s_in == I_CONNECTING && s_inLe && s_leHandle == HCI_CON_HANDLE_INVALID) gap_connect_cancel();
    if (s_hidCid) hid_host_disconnect(s_hidCid);
    if (s_hidsCid) hids_host_disconnect(s_hidsCid);
    if (s_leHandle != HCI_CON_HANDLE_INVALID) gap_disconnect(s_leHandle);
    inputRelease();
    s_in = I_IDLE;
}

bool inputRemembered(uint8_t addr[6], char* name, int cap) {
    bd_addr_t a; bool le; uint8_t t;
    if (!loadInput(a, le, t, name, cap)) return false;
    memcpy(addr, a, 6);
    return true;
}

void bootTick() {
    // ~3 s after boot, once: power up if an input device is remembered.
    static bool done = false;
    if (done || s_state != S_OFF || nowMs() < 3000) return;
    done = true;
    bd_addr_t a; bool le; uint8_t t; char nm[32];
    if (!loadInput(a, le, t, nm, sizeof(nm))) return;
    blog("remembered input %s - starting Bluetooth", nm);
    powerOnAsync();
}

AudioState audioState() { return s_audio; }
const char* audioName() { return s_audioName; }

bool audioConnect(const uint8_t addr[6], const char* name) {
    if (!ensureUp()) return false;
    if (s_busy) finish("stopped");
    if (s_cid) audioDisconnect();
    memcpy(s_audioAddr, addr, 6);
    snprintf(s_audioName, sizeof(s_audioName), "%s", name && name[0] ? name : bd_addr_to_str(s_audioAddr));
    uint16_t cid = 0;
    const uint8_t rc = a2dp_source_establish_stream(s_audioAddr, &cid);
    if (rc != ERROR_CODE_SUCCESS) {
        blog("connect failed to start, status 0x%02x", rc);
        s_audio = A_FAILED;
        return false;
    }
    s_cid = cid;
    s_audio = A_CONNECTING;
    blog("connecting to %s...", s_audioName);
    return true;
}

void audioDisconnect() {
    ringOff();
    if (s_cid) a2dp_source_disconnect(s_cid);
    s_audio = A_IDLE;
}

bool remembered(uint8_t addr[6], char* name, int cap) {
    if (!FileUtils::fsMount) return false;
    FIL f;
    if (f_open(&f, BT_AUDIO_CFG, FA_READ) != FR_OK) return false;
    char line[80] = {0};
    UINT n = 0;
    f_read(&f, line, sizeof(line) - 1, &n);
    f_close(&f);
    line[n] = 0;
    char* tab = strchr(line, '\t');
    if (!tab) return false;
    *tab = 0;
    char* nl = strpbrk(tab + 1, "\r\n");
    if (nl) *nl = 0;
    bd_addr_t a;
    if (!sscanf_bd_addr(line, a)) return false;
    memcpy(addr, a, 6);
    snprintf(name, cap, "%s", tab + 1);
    return true;
}

void forget(const uint8_t addr[6]) {
    bd_addr_t a;
    memcpy(a, addr, 6);
    if (s_cid && memcmp(a, s_audioAddr, 6) == 0) audioDisconnect();
    if (ensureUp()) gap_drop_link_key_for_bd_addr(a);   // the key lives in the stack's TLV
    uint8_t ra[6]; char rn[32];
    if (remembered(ra, rn, sizeof(rn)) && memcmp(ra, a, 6) == 0) f_unlink(BT_AUDIO_CFG);
    bd_addr_t ia; bool le; uint8_t t;
    if (loadInput(ia, le, t, rn, sizeof(rn)) && memcmp(ia, a, 6) == 0) {
        if ((s_in == I_READY || s_in == I_CONNECTING) && memcmp(s_inAddr, a, 6) == 0) inputDisconnect();
        if (le && s_state == S_UP) { gap_auto_connection_stop((bd_addr_type_t)t, ia); gap_delete_bonding((bd_addr_type_t)t, ia); }
        f_unlink(BT_INPUT_CFG);
    }
    blog("forgot %s", bd_addr_to_str(a));
}

bool streaming() { return s_audio == A_STREAMING; }

// Called once per frame (netBackgroundTick) and from the frame-pacing waits: the
// stream needs a send opportunity every few ms, more than one radio poll a frame.
void pump() {
    if (s_audio != A_STREAMING) return;
    static uint32_t last = 0;
    const uint32_t now = time_us_32();
    if (now - last < 2000) return;
    last = now;
    audioWork();
    WifiNet::poll();
}

const char* stateStr() {
    switch (s_state) {
        case S_STARTING: return "starting";
        case S_UP:       return "up";
        case S_FAILED:   return "failed";
        default:         return "off";
    }
}

}  // namespace BtHost

#endif  // PICOSPECCY_BT
