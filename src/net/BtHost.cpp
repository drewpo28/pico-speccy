// On-chip Bluetooth: discovery, A2DP source, HID host (classic + BLE). See BtHost.h.
#if PICOSPECCY_WIFI

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
#include "hci_dump.h"

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

void audioResume();   // reconnect the speaker that played before a reboot
void audioDrop();     // audioDisconnect without forgetting the "audio on" marker
bool audioEstablish();          // defined with the public audio calls below
namespace {

#define BT_AUDIO_CFG CONFIG_DIR "/bt_audio.txt"
#define BT_INPUT_CFG CONFIG_DIR "/bt_input.txt"
// Present while the remembered speaker is in use: the next boot reconnects it.
#define BT_AUDIO_ON  CONFIG_DIR "/bt_audio_on"

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
bool     s_audioPending = false;   // a switch waits for the old connection to close
btstack_timer_source_t s_audioRetryT;   // busy-connect retries (audioEstablish)
uint8_t  s_audioRetries = 0;
bd_addr_t  s_audioAddr;
uint16_t   s_cid  = 0;
hci_con_handle_t s_audioHandle = HCI_CON_HANDLE_INVALID;   // the sink's ACL link
uint8_t    s_seid = 0;        // local stream endpoint

constexpr int SBC_STORAGE = 1030;
// 262 ms of 31250 Hz stereo (32 KB, butter PSRAM when there is one). The radio
// can hold the send slot for 40-120 ms when WiFi or the LE background connect
// takes its airtime (hw 2026-10-30, JBL GO 2): with the old 65 ms ring the
// latency cap threw that audio away and the sink filled the hole. Steady state
// the ring is near empty, so this costs no latency; after a stall the backlog
// goes out back to back.
constexpr int RING_FRAMES = 8192;
void leAudioMode(bool on);         // defined after leAutoConnect

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
    uint32_t skips, skipSmp;        // latency-cap jumps and the samples they threw away
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
    leAudioMode(false);
}

void streamFree() {
    ringOff();
    if (s_st) { Buffer::pfree(s_st); s_st = nullptr; }
}

// ── The added-device lists ───────────────────────────────────────────────────
// One file per kind, one device per line, most recently used first:
//   audio "addr\tname", input "addr\tC|L\taddrType\tname".
// A file written by the one-device firmware is simply a list of one.
const char* listPath(bool input) { return input ? BT_INPUT_CFG : BT_AUDIO_CFG; }

int listLoad(bool input, Known* out, int max) {
    if (!FileUtils::fsMount) return 0;
    FIL f;
    if (f_open(&f, listPath(input), FA_READ) != FR_OK) return 0;
    char buf[KNOWN_MAX * 72 + 8];
    UINT got = 0;
    f_read(&f, buf, sizeof(buf) - 1, &got);
    f_close(&f);
    buf[got] = 0;
    int n = 0;
    char* line = buf;
    while (line && *line && n < max) {
        char* next = strpbrk(line, "\r\n");
        if (next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; }
        Known k = {};
        char* f1 = strchr(line, '\t');
        if (f1) {
            *f1++ = 0;
            bd_addr_t a;
            bool ok = sscanf_bd_addr(line, a) != 0;
            const char* nm = f1;
            if (ok && input) {
                char* f2 = strchr(f1, '\t');
                char* f3 = f2 ? strchr(f2 + 1, '\t') : nullptr;
                if (!f3) ok = false;
                else {
                    *f2++ = 0; *f3++ = 0;
                    k.le = f1[0] == 'L';
                    k.addrType = (uint8_t)(f2[0] - '0');
                    nm = f3;
                }
            }
            static const uint8_t kZero[6] = {0};
            if (ok && memcmp(a, kZero, 6) == 0) ok = false;     // a bogus entry of the 1st build
            if (ok) {
                memcpy(k.addr, a, 6);
                snprintf(k.name, sizeof(k.name), "%s", nm);
                out[n++] = k;
            }
        }
        line = next;
    }
    return n;
}

void listSave(bool input, const Known* l, int n) {
    if (!FileUtils::fsMount) return;
    if (n <= 0) { f_unlink(listPath(input)); return; }
    FIL f;
    if (f_open(&f, listPath(input), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    for (int i = 0; i < n; i++) {
        char line[80];
        bd_addr_t a;
        memcpy(a, l[i].addr, 6);
        const int len = input
            ? snprintf(line, sizeof(line), "%s\t%c\t%u\t%s\n", bd_addr_to_str(a),
                       l[i].le ? 'L' : 'C', (unsigned)l[i].addrType, l[i].name)
            : snprintf(line, sizeof(line), "%s\t%s\n", bd_addr_to_str(a), l[i].name);
        UINT w; f_write(&f, line, len, &w);
    }
    f_close(&f);
}

// Put `k` at the head of its list (an entry with the same address moves there).
void listUpsert(bool input, const Known& k) {
    Known l[KNOWN_MAX];
    int n = listLoad(input, l, KNOWN_MAX);
    int at = n;
    for (int i = 0; i < n; i++) if (memcmp(l[i].addr, k.addr, 6) == 0) { at = i; break; }
    if (at == n && n == KNOWN_MAX) at = n - 1;          // full: the oldest goes
    else if (at == n) n++;
    for (int i = at; i > 0; i--) l[i] = l[i - 1];
    l[0] = k;
    listSave(input, l, n);
}

// Drop `addr` from a list; the entry it had goes to *was (when given).
bool listRemove(bool input, const uint8_t addr[6], Known* was) {
    Known l[KNOWN_MAX];
    int n = listLoad(input, l, KNOWN_MAX);
    for (int i = 0; i < n; i++)
        if (memcmp(l[i].addr, addr, 6) == 0) {
            if (was) *was = l[i];
            for (int j = i; j + 1 < n; j++) l[j] = l[j + 1];
            listSave(input, l, n - 1);
            return true;
        }
    return false;
}

// Is `addr` in the input list, with this link type? (fills *k when it is)
bool inputKnown(const uint8_t addr[6], bool le, Known* k) {
    Known l[KNOWN_MAX];
    const int n = listLoad(true, l, KNOWN_MAX);
    for (int i = 0; i < n; i++)
        if (l[i].le == le && memcmp(l[i].addr, addr, 6) == 0) { if (k) *k = l[i]; return true; }
    return false;
}

// The "audio on" marker: set when a stream starts, removed when the user
// disconnects. A reboot (F12, a machine switch) then brings the speaker back.
void audioOnSave(bool on) {
    if (!FileUtils::fsMount) return;
    if (!on) { f_unlink(BT_AUDIO_ON); return; }
    FIL f;
    if (f_open(&f, BT_AUDIO_ON, FA_WRITE | FA_CREATE_NEW) == FR_OK) f_close(&f);
}
bool audioOnSaved() {
    FILINFO fi;
    return FileUtils::fsMount && f_stat(BT_AUDIO_ON, &fi) == FR_OK;
}

void saveRemembered() {
    static const uint8_t kZero[6] = {0};
    if (memcmp(s_audioAddr, kZero, 6) == 0) return;
    Known k = {};
    memcpy(k.addr, s_audioAddr, 6);
    snprintf(k.name, sizeof(k.name), "%s", s_audioName);
    listUpsert(false, k);
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
    // Latency cap, emergency only: a stall of ~245 ms (a long SD load, a radio
    // that stops granting slots) must not leave the stream permanently behind -
    // keep the newest ~65 ms. Shorter stalls are sent late instead of dropped.
    if (w - r > (uint32_t)(RING_FRAMES - 512)) {
        st->skips++;
        st->skipSmp += (w - r) - 2048;
        r = w - 2048;
    }
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
        Debug::log("BT audio: %u pkt %u sbc/5s, enc %u ms/5s, skip %u/%u smp, drops %u, ring %u, bad idx %u/%u",
                   (unsigned)st->pkts, (unsigned)st->frames, (unsigned)(st->encUs / 1000),
                   (unsigned)st->skips, (unsigned)st->skipSmp,
                   (unsigned)pcm_bt_drops, (unsigned)(pcm_bt_w - pcm_bt_r),
                   (unsigned)cybt_bad_index_reads, (unsigned)cybt_bad_index_giveups);
        st->pkts = st->frames = st->encUs = 0;
        st->skips = st->skipSmp = 0;
        st->statT = now;
    }
}

bool streamStart() {
    if (!s_st) {
        s_st = (Stream*)Buffer::palloc(sizeof(Stream), Buffer::NEED_POINTER | Buffer::HOT_SRAM);
        if (!s_st) { blog("no memory for the SBC encoder (%u B)", (unsigned)sizeof(Stream)); return false; }
    }
    if (!s_ring) {
        s_ring = (int16_t*)Buffer::palloc(RING_FRAMES * 4, Buffer::NEED_POINTER | Buffer::PREFER_PSRAM);
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
            s_audioHandle = a2dp_subevent_signaling_connection_established_get_con_handle(p);
            {
                // The sink may have connected to US (a speaker that remembers this
                // source pages it when switched on): then nothing set the address
                // and name, and the zeros went into the device list.
                bd_addr_t a;
                a2dp_subevent_signaling_connection_established_get_bd_addr(p, a);
                const bool incoming = s_audio != A_CONNECTING || bd_addr_cmp(a, s_audioAddr) != 0;
                if (incoming) {
                    memcpy(s_audioAddr, a, 6);
                    s_audioName[0] = 0;
                    Known l[KNOWN_MAX];                          // a name we already know
                    const int n = listLoad(false, l, KNOWN_MAX);
                    for (int i = 0; i < n; i++)
                        if (memcmp(l[i].addr, a, 6) == 0) { snprintf(s_audioName, sizeof(s_audioName), "%s", l[i].name); break; }
                    for (int i = 0; i < s_ndev && !s_audioName[0]; i++)  // ...or one the last scan saw
                        if (memcmp(s_dev[i].addr, a, 6) == 0 && s_dev[i].name[0])
                            snprintf(s_audioName, sizeof(s_audioName), "%s", s_dev[i].name);
                    if (!s_audioName[0]) snprintf(s_audioName, sizeof(s_audioName), "%s", bd_addr_to_str(s_audioAddr));
                    s_audio = A_CONNECTING;
                    blog("%s connected to us", s_audioName);
                }
            }
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
            leAudioMode(true);
            saveRemembered();
            audioOnSave(true);
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
            if (!s_audioPending && (s_audio == A_STREAMING || s_audio == A_CONNECTING)) s_audio = A_IDLE;
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            blog("disconnected");
            streamFree();
            s_cid = 0;
            s_audioHandle = HCI_CON_HANDLE_INVALID;
            if (s_audioPending) {                  // switching: now the new device
                s_audioPending = false;
                audioEstablish();
                break;
            }
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
// Pairing a NEW LE device: a link that drops before the HID service is reached
// (0x3E "not established" is common on the first connect) is retried once.
bool       s_pairNew   = false;
uint8_t    s_pairTry   = 0;         // 0 = first attempt, 1 = the retry
uint32_t   s_leConnMs  = 0;         // when the current LE link came up
uint16_t   s_hidCid  = 0;           // classic hid_host
hid_protocol_mode_t s_hidMode = HID_PROTOCOL_MODE_REPORT;
bool       s_hidDescOk = false;
hci_con_handle_t s_leHandle = HCI_CON_HANDLE_INVALID;
uint16_t   s_hidsCid = 0;           // BLE hids_host
btstack_packet_callback_registration_t s_sm_reg;
// Descriptor storage the two hosts keep the devices' HID descriptors in.
uint8_t    s_hidDescStore[512];
uint8_t    s_hidsDescStore[512];

// What the menu shows about the connected input device (Bluetooth > Input, the
// right pane). Battery and the Device Information strings are BLE-only: read
// once the HID service is up (DIS first, then the Battery Service, which keeps
// the level fresh by notification or a 60 s poll).
InputInfo s_info;
uint16_t  s_basCid = 0;

void infoReset() {
    memset(&s_info, 0, sizeof(s_info));
    s_info.battery = -1;
}

// "mouse" / "keyboard" / ... from the descriptor's first top-level usage.
void descKind(const uint8_t* d, uint16_t len) {
    s_info.descLen = len;
    uint16_t page = 0, usage = 0xFFFF;
    for (uint16_t i = 0; d && i < len;) {
        const uint8_t pfx = d[i];
        if (pfx == 0xFE) { if (i + 2 >= len) break; i += 3 + d[i + 1]; continue; }   // long item
        static const uint8_t kSz[4] = { 0, 1, 2, 4 };
        const uint8_t sz = kSz[pfx & 3];
        if (i + 1 + sz > len) break;
        uint32_t v = 0;
        for (uint8_t b = 0; b < sz; b++) v |= (uint32_t)d[i + 1 + b] << (8 * b);
        const uint8_t tag = pfx & 0xFC;
        if (tag == 0x04) page = (uint16_t)v;                         // Usage Page
        else if (tag == 0x08 && usage == 0xFFFF) usage = (uint16_t)v; // first Usage
        else if (tag == 0xA0) break;                                  // Collection
        i += 1 + sz;
    }
    const char* k = "HID device";
    if (page == 0x01) {
        switch (usage) {
            case 0x02: k = "mouse"; break;
            case 0x06: k = "keyboard"; break;
            case 0x04: k = "joystick"; break;
            case 0x05: k = "gamepad"; break;
            default: break;
        }
    } else if (page == 0x0C) k = "media keys";
    snprintf(s_info.kind, sizeof(s_info.kind), "%s", k);
}

void disHandler(uint8_t, uint16_t, uint8_t* p, uint16_t);

void batHandler(uint8_t, uint16_t, uint8_t* p, uint16_t) {
    if (hci_event_packet_get_type(p) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(p)) {
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_CONNECTED: {
            const uint8_t st = gattservice_subevent_battery_service_connected_get_status(p);
            if (st != ERROR_CODE_SUCCESS) { blog("no battery service (0x%02x)", st); s_basCid = 0; }
            else {
                blog("battery service: %u instance(s), poll mask %02X",
                     gattservice_subevent_battery_service_connected_get_num_instances(p),
                     gattservice_subevent_battery_service_connected_get_poll_bitmap(p));
                // A notifying service only reports a CHANGE of the level (hours on a
                // mouse): read the current value once.
                const uint8_t rc = battery_service_client_read_battery_level(s_basCid, 0);
                if (rc != ERROR_CODE_SUCCESS) blog("battery read failed to start 0x%02x", rc);
            }
            // Then the Device Information strings (one GATT query at a time).
            if (s_leHandle != HCI_CON_HANDLE_INVALID) {
                const uint8_t rc = device_information_service_client_query(s_leHandle, disHandler);
                if (rc != ERROR_CODE_SUCCESS) blog("device information: start failed 0x%02x", rc);
            }
            break;
        }
        case GATTSERVICE_SUBEVENT_BATTERY_SERVICE_LEVEL:
            if (gattservice_subevent_battery_service_level_get_att_status(p) == ATT_ERROR_SUCCESS) {
                const int8_t lv = (int8_t)gattservice_subevent_battery_service_level_get_level(p);
                if (lv != s_info.battery) blog("battery %d%%", lv);
                s_info.battery = lv;
            }
            break;
        default: break;
    }
}

void disHandler(uint8_t, uint16_t, uint8_t* p, uint16_t) {
    if (hci_event_packet_get_type(p) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(p)) {
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_MANUFACTURER_NAME:
            if (gattservice_subevent_device_information_manufacturer_name_get_att_status(p) == ATT_ERROR_SUCCESS)
                snprintf(s_info.maker, sizeof(s_info.maker), "%s", gattservice_subevent_device_information_manufacturer_name_get_value(p));
            break;
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_MODEL_NUMBER:
            if (gattservice_subevent_device_information_model_number_get_att_status(p) == ATT_ERROR_SUCCESS)
                snprintf(s_info.model, sizeof(s_info.model), "%s", gattservice_subevent_device_information_model_number_get_value(p));
            break;
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_FIRMWARE_REVISION:
            if (gattservice_subevent_device_information_firmware_revision_get_att_status(p) == ATT_ERROR_SUCCESS)
                snprintf(s_info.fw, sizeof(s_info.fw), "%s", gattservice_subevent_device_information_firmware_revision_get_value(p));
            break;
        case GATTSERVICE_SUBEVENT_DEVICE_INFORMATION_DONE:
            blog("device information done (0x%02x)", gattservice_subevent_device_information_done_get_att_status(p));
            break;
        default: break;
    }
}

// After the HID service: the battery first (it is what the menu wants most),
// then the Device Information strings once the battery client has settled.
void infoQueryStart() {
    if (s_leHandle == HCI_CON_HANDLE_INVALID) return;
    if (s_basCid) { battery_service_client_disconnect(s_basCid); s_basCid = 0; }
    const uint8_t rc = battery_service_client_connect(s_leHandle, batHandler, 60000, &s_basCid);
    if (rc != ERROR_CODE_SUCCESS) {
        blog("battery service: start failed 0x%02x", rc);
        s_basCid = 0;
        const uint8_t r2 = device_information_service_client_query(s_leHandle, disHandler);
        if (r2 != ERROR_CODE_SUCCESS) blog("device information: start failed 0x%02x", r2);
    }
}

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

// The first reports of a connection go to the log (raw bytes + what came out of
// them): "connected but does nothing" is otherwise undiagnosable.
uint8_t s_repLog = 0;

// One input report, through the device's descriptor (or the boot one).
void inputReport(const uint8_t* desc, uint16_t dlen, const uint8_t* rep, uint16_t rlen) {
    if (!desc || !dlen || !rlen) return;
    char raw[3 * 12 + 1];
    if (s_repLog < 12) {
        int o = 0;
        for (int i = 0; i < rlen && i < 12; i++) o += snprintf(raw + o, sizeof(raw) - o, "%02X ", rep[i]);
        raw[o] = 0;
    }
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
    if (s_repLog < 12) {
        s_repLog++;
        blog("report %u B: %s-> %s%s%s btn=%lX dx=%ld dy=%ld", (unsigned)rlen, raw,
             kbd ? "kbd " : "", rel ? "rel " : "", (absx || absy || hat) ? "abs " : "",
             (unsigned long)buttons, (long)dx, (long)dy);
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
    Known k = {};
    memcpy(k.addr, s_inAddr, 6);
    k.le = s_inLe;
    k.addrType = s_inAddrType;
    snprintf(k.name, sizeof(k.name), "%s", s_inName);
    listUpsert(true, k);
}

// The added LE devices reconnect when they advertise: keep the controller
// listening for them (BTstack's auto-connect = whitelist + background connect).
void leAutoConnect(bool on) {
    Known l[KNOWN_MAX];
    const int n = listLoad(true, l, KNOWN_MAX);
    for (int i = 0; i < n; i++) {
        if (!l[i].le) continue;
        bd_addr_t a;
        memcpy(a, l[i].addr, 6);
        if (on) gap_auto_connection_start((bd_addr_type_t)l[i].addrType, a);
        else    gap_auto_connection_stop((bd_addr_type_t)l[i].addrType, a);
    }
}

// With a BLE device connected the CYW43 hands the A2DP link about one ACL packet
// per LE connection event (hw 2026-10-30, FY-128 + JBL GO 2): at 48.75 ms the
// stream got ~22 packets/s, at 15 ms ~66 against the 69 it needs. While audio
// streams the LE link is therefore kept at 7.5 ms with NO slave latency, so its
// events never thin out.
void leFastLink() {
    if (s_leHandle == HCI_CON_HANDLE_INVALID) return;
    const int rc = gap_update_connection_parameters(s_leHandle, 6, 6, 0, 300);
    blog("LE link %04x: asking 7.5 ms, latency 0 while audio streams (rc %d)", (unsigned)s_leHandle, rc);
}

// While A2DP streams: a connected BLE input device keeps short, latency-free
// connection events (leFastLink), and BTstack refuses its requests for slave
// latency. Undone when the stream stops.
void leAudioMode(bool on) {
    static bool s_on = false;
    if (on == s_on || s_state != S_UP) { if (s_state != S_UP) s_on = false; return; }
    s_on = on;
    le_connection_parameter_range_t r;
    gap_get_connection_parameter_range(&r);
    r.le_conn_latency_max = on ? 0 : 500;
    gap_set_connection_parameter_range(&r);
    if (on) leFastLink();
}

void inputReady() {
    s_pairTry = 0;
    s_pairNew = false;
    s_in = I_READY;
    saveInput();
    blog("%s ready (%s)", s_inName, s_inLe ? "BLE" : "classic");
    // A BLE device that (re)connected while audio already streams: its link
    // came up at the default 30 ms / latency 4, and the stream got ~30 packets
    // a second off it (hw 2026-10-30, diag7). leAudioMode ran before it existed.
    if (s_inLe && s_audio == A_STREAMING) leFastLink();
}

void inputClosed(const char* why) {
    inputRelease();
    s_basCid = 0;              // the battery client drops itself with the link
    infoReset();
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
            Known kn = {};
            const bool known = inputKnown(a, false, &kn) ||
                               (s_in == I_CONNECTING && !s_inLe && bd_addr_cmp(s_inAddr, a) == 0);
            const uint16_t cid = hid_subevent_incoming_connection_get_hid_cid(p);
            if (!known || s_hidCid) { hid_host_decline_connection(cid); break; }
            if (s_in != I_CONNECTING) { memcpy(s_inAddr, a, 6); snprintf(s_inName, sizeof(s_inName), "%s", kn.name); s_inLe = false; }
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
            if (s_hidDescOk) {
                axesScan(hid_descriptor_storage_get_descriptor_data(s_hidCid),
                         hid_descriptor_storage_get_descriptor_len(s_hidCid));
                descKind(hid_descriptor_storage_get_descriptor_data(s_hidCid),
                         hid_descriptor_storage_get_descriptor_len(s_hidCid));
            }
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

void hidsHandler(uint8_t, uint16_t, uint8_t* p, uint16_t) {
    // No packet-type test: hids_host hands its HID reports over with packet type
    // HCI_EVENT_GATTSERVICE_META (hids_host_handle_notification_event), not
    // HCI_EVENT_PACKET — testing for the latter silently dropped every report.
    if (hci_event_packet_get_type(p) != HCI_EVENT_GATTSERVICE_META) return;
    switch (hci_event_gattservice_meta_get_subevent_code(p)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            if (gattservice_subevent_hid_service_connected_get_status(p) != ERROR_CODE_SUCCESS) {
                blog("HID service not found (0x%02x)", gattservice_subevent_hid_service_connected_get_status(p));
                s_in = I_FAILED;
                gap_disconnect(s_leHandle);
                break;
            }
        {
            const uint8_t* dd = hids_host_descriptor_storage_get_descriptor_data(s_hidsCid, 0);
            const uint16_t dl = hids_host_descriptor_storage_get_descriptor_len(s_hidsCid, 0);
            blog("HID descriptor %u B%s, report IDs %s", (unsigned)dl,
                 dl >= sizeof(s_hidsDescStore) ? " (TRUNCATED)" : "",
                 dl && btstack_hid_report_id_declared(dd, dl) ? "yes" : "no");
            axesScan(dd, dl);
            descKind(dd, dl);
            s_repLog = 0;
            inputReady();
            infoQueryStart();
        }
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            const uint8_t si = gattservice_subevent_hid_report_get_service_index(p);
            const uint8_t* dd = hids_host_descriptor_storage_get_descriptor_data(s_hidsCid, si);
            const uint16_t dl = hids_host_descriptor_storage_get_descriptor_len(s_hidsCid, si);
            const uint8_t* r = gattservice_subevent_hid_report_get_report(p);
            uint16_t rl = gattservice_subevent_hid_report_get_report_len(p);
            // hids_host always puts the report ID in front, also for a descriptor
            // that declares none — then the parser would read every field one
            // byte off. Strip it there.
            if (rl && dl && !btstack_hid_report_id_declared(dd, dl)) { r++; rl--; }
            inputReport(dd, dl, r, rl);
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
        case SM_EVENT_SECURITY_REQUEST:
            blog("the device asks for security (auth %02X)", sm_event_security_request_get_auth_req(p));
            break;
        case SM_EVENT_PAIRING_STARTED:
            blog("pairing...");
            break;
        case SM_EVENT_PAIRING_COMPLETE: {
            const uint8_t st = sm_event_pairing_complete_get_status(p);
            if (st == ERROR_CODE_SUCCESS) { blog("paired"); go = true; break; }
            // 0x13 = the link dropped during pairing (the cause is on the
            // disconnect line); 0x05 = SMP failure, the reason says why.
            blog("pairing failed (status 0x%02x, reason 0x%02x)", st, sm_event_pairing_complete_get_reason(p));
            // leDisconnected decides: retry, or fail.
            if (sm_event_pairing_complete_get_handle(p) == s_leHandle && s_leHandle != HCI_CON_HANDLE_INVALID)
                gap_disconnect(s_leHandle);
            break;
        }
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
    Known kn = {};
    const bool pairing = s_in == I_CONNECTING && s_inLe && bd_addr_cmp(a, s_inAddr) == 0;
    const bool known   = inputKnown(a, true, &kn);
    if (!pairing && !known) return;        // not ours (the audio side never uses LE)
    // One input device at a time: a second added one that advertises meanwhile is dropped.
    if (!pairing && (s_in == I_READY || s_in == I_CONNECTING)) {
        gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(p));
        return;
    }
    if (!pairing) { memcpy(s_inAddr, a, 6); s_inAddrType = kn.addrType; snprintf(s_inName, sizeof(s_inName), "%s", kn.name); s_inLe = true; }
    s_leHandle = gap_subevent_le_connection_complete_get_connection_handle(p);
    s_leConnMs = nowMs();
    s_in = I_CONNECTING;
    leAutoConnect(false);
    blog("%s connected, securing the link...", s_inName);
    sm_request_pairing(s_leHandle);         // pairs, or re-encrypts a bonded device
}

void leDisconnected(hci_con_handle_t h, uint8_t reason) {
    if (h != s_leHandle) return;
    blog("link dropped after %u ms, reason 0x%02x%s", (unsigned)(nowMs() - s_leConnMs), reason,
         reason == 0x08 ? " (timeout: too far / weak signal)" :
         reason == 0x13 ? " (by the device)" :
         reason == 0x3E ? " (link not established)" : "");
    if (s_in == I_CONNECTING && s_inLe && s_pairNew && !s_hidsCid && s_pairTry == 0) {
        // A new device dropped before it was set up: one more try.
        s_pairTry = 1;
        s_leHandle = HCI_CON_HANDLE_INVALID;
        gap_delete_bonding((bd_addr_type_t)s_inAddrType, s_inAddr);
        const uint8_t rc = gap_connect(s_inAddr, (bd_addr_type_t)s_inAddrType);
        if (rc == ERROR_CODE_SUCCESS) { blog("retrying..."); return; }
        blog("retry failed to start, status 0x%02x", rc);
    }
    s_pairTry = 0;
    if (s_in == I_CONNECTING) s_in = I_FAILED;
    s_pairNew = false;
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
    // BTstack defaults to Secure Connections ONLY when ENABLE_LE_SECURE_CONNECTIONS
    // is set, and then refuses every legacy-pairing device with reason 0x03 —
    // which is most cheap BLE mice (FY-128 answers io=03 auth=01). SC is still
    // asked for and used whenever the device supports it.
    sm_set_secure_connections_only_mode(false);
    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    s_sm_reg.callback = &smHandler;
    sm_add_event_handler(&s_sm_reg);
    gatt_client_init();
    hids_host_init(s_hidsDescStore, sizeof(s_hidsDescStore));
    battery_service_client_init();
    device_information_service_client_init();
    infoReset();

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
                audioResume();                    // the speaker that played before a reboot
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
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS: {
            // The radio sometimes reports the sink's completed packets with bit 0
            // of the handle cleared (000b -> 000a, 000d -> 000c; hw 2026-10-30,
            // every A2DP collapse). BTstack drops a completion for an unknown
            // handle, so those packets stay "in flight" for ever and the stream
            // runs on what is left of the controller's 8 ACL buffers: ~4 packets a
            // second until a reboot. Credit them to the sink's link when its handle
            // differs only in bit 0.
            if (s_audioHandle == HCI_CON_HANDLE_INVALID) break;
            const int n = packet[2];
            for (int i = 0; i < n; i++) {
                const uint16_t h = little_endian_read_16(packet, 3 + i * 4) & 0x0fff;
                const uint16_t c = little_endian_read_16(packet, 5 + i * 4);
                if ((h ^ s_audioHandle) != 1 || hci_connection_for_handle(h)) continue;
                hci_connection_t* ac = hci_connection_for_handle(s_audioHandle);
                if (!ac) continue;
                const unsigned before = ac->num_packets_sent;
                ac->num_packets_sent = before >= c ? (uint8_t)(before - c) : 0;
                blog("repaired %u completed packets: handle %04x -> %04x (outstanding %u -> %u)",
                     (unsigned)c, (unsigned)h, (unsigned)s_audioHandle, before,
                     (unsigned)ac->num_packets_sent);
                if (s_st && s_st->readyToSend)
                    a2dp_source_stream_endpoint_request_can_send_now(s_cid, s_seid);
            }
            break;
        }
        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                // The device renegotiated its link while audio streams: back to 7.5 ms.
                const uint16_t iv = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
                if (s_audio == A_STREAMING &&
                    (iv > 6 || hci_subevent_le_connection_update_complete_get_conn_latency(packet) != 0))
                    leFastLink();
            }
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                leConnected(packet);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            leDisconnected(hci_event_disconnection_complete_get_connection_handle(packet),
                           hci_event_disconnection_complete_get_reason(packet));
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

// SMP trace: every Security Manager PDU (L2CAP CID 6) in both directions goes to
// the log, decoded enough to tell WHO refused pairing and what each side asked
// for. BTstack's own error messages come along too. Cheap: one CID test per ACL
// packet, and SMP is a handful of packets per pairing.
void smpField(char*& o, char* end, const char* name, unsigned v) {
    const int n = snprintf(o, end - o, " %s=%02X", name, v);
    if (n > 0 && o + n < end) o += n;
}
void dumpPacket(uint8_t type, uint8_t in, uint8_t* p, uint16_t len) {
    if (type != HCI_ACL_DATA_PACKET || len < 9) return;
    if (little_endian_read_16(p, 6) != L2CAP_CID_SECURITY_MANAGER_PROTOCOL) return;
    const uint8_t* d = p + 8;
    const int n = len - 8;
    char line[110];
    char* o = line;
    char* end = line + sizeof(line);
    static const char* const kOp[] = { "?", "PairReq", "PairRsp", "Confirm", "Random", "Failed",
                                       "EncInfo", "MasterId", "IdInfo", "IdAddr", "SignInfo",
                                       "SecReq", "PubKey", "DHKeyChk", "Keypress" };
    const uint8_t op = d[0];
    o += snprintf(o, end - o, "SMP %s %s", in ? "<" : ">", op < 15 ? kOp[op] : "?");
    if ((op == 1 || op == 2) && n >= 7) {
        smpField(o, end, "io", d[1]); smpField(o, end, "oob", d[2]); smpField(o, end, "auth", d[3]);
        smpField(o, end, "key", d[4]); smpField(o, end, "ikd", d[5]); smpField(o, end, "rkd", d[6]);
    } else if (op == 5 && n >= 2) {
        smpField(o, end, "reason", d[1]);
    } else if (op == 11 && n >= 2) {
        smpField(o, end, "auth", d[1]);
    }
    blog("%s", line);
}
void dumpMessage(int level, const char* fmt, va_list ap) {
    if (level != HCI_DUMP_LOG_LEVEL_ERROR) return;
    char line[110];
    vsnprintf(line, sizeof(line), fmt, ap);
    // An audio switch retries while the old link closes; the controller refuses
    // each Create Connection meanwhile ("Command packet sent failed, error = 86")
    // — expected, and already said once by "waiting for the previous connection".
    if (s_audioRetries && strstr(line, "Command packet sent failed")) return;
    blog("btstack: %s", line);
}
const hci_dump_t s_dump = { nullptr, dumpPacket, dumpMessage };

// Start the controller (BT firmware download) without waiting. False = no radio.
uint32_t s_powerT0 = 0;
bool powerOnAsync() {
    if (s_state != S_OFF) return s_state != S_FAILED;
    if (!WifiNet::ready() || !WifiNet::busUp()) {
        s_state = S_FAILED;
        blog("radio not available");
        return false;
    }
    hci_dump_init(&s_dump);
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
    leAudioMode(false);   // the user is connecting a device now: normal LE rules
    leAutoConnect(false);
    memcpy(s_inAddr, d.addr, 6);
    s_inLe = d.le;
    s_inAddrType = d.addrType;
    snprintf(s_inName, sizeof(s_inName), "%s", d.name[0] ? d.name : bd_addr_to_str(s_inAddr));
    s_pairNew = d.le && !inputKnown(d.addr, true, nullptr);
    s_pairTry = 0;
    s_in = I_CONNECTING;
    if (d.le && !s_pairNew) {
        // An added LE device: the background auto-connect already waits for it
        // to advertise (a direct gap_connect beside it is refused, "le connect
        // already active"). Keep it running; leConnected takes it from here.
        leAutoConnect(true);
        blog("waiting for %s to advertise...", s_inName);
        return true;
    }
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
    const bool waitingLe = s_in == I_CONNECTING && s_inLe && s_leHandle == HCI_CON_HANDLE_INVALID;
    if (waitingLe && s_pairNew) gap_connect_cancel();
    if (s_hidCid) hid_host_disconnect(s_hidCid);
    if (s_hidsCid) hids_host_disconnect(s_hidsCid);
    if (s_leHandle != HCI_CON_HANDLE_INVALID) gap_disconnect(s_leHandle);
    inputRelease();
    if (s_basCid) { battery_service_client_disconnect(s_basCid); s_basCid = 0; }
    infoReset();
    s_pairTry = 0;
    s_pairNew = false;
    s_in = I_IDLE;
}

bool inputRemembered(uint8_t addr[6], char* name, int cap) {
    Known k;
    if (listLoad(true, &k, 1) < 1) return false;
    memcpy(addr, k.addr, 6);
    snprintf(name, cap, "%s", k.name);
    return true;
}

const InputInfo* inputInfo() {
    return (s_in == I_READY || s_in == I_CONNECTING) ? &s_info : nullptr;
}

bool audioInfo(AudioInfo& a) {
    if (s_audio != A_STREAMING) return false;
    a.fs = s_cfg.fs;
    a.bitpool = s_cfg.bitpool;
    a.blocks = s_cfg.blocks;
    a.subbands = s_cfg.subbands;
    a.joint = s_cfg.cm == SBC_CHANNEL_MODE_JOINT_STEREO;
    return true;
}

int known(bool input, Known* out, int max) {
    if (max > KNOWN_MAX) max = KNOWN_MAX;
    return listLoad(input, out, max);
}

bool isKnown(bool input, const uint8_t addr[6]) {
    Known l[KNOWN_MAX];
    const int n = listLoad(input, l, KNOWN_MAX);
    for (int i = 0; i < n; i++) if (memcmp(l[i].addr, addr, 6) == 0) return true;
    return false;
}

bool connectedTo(bool input, const uint8_t addr[6]) {
    if (input) return (s_in == I_READY || s_in == I_CONNECTING) && memcmp(s_inAddr, addr, 6) == 0;
    return (s_audio == A_STREAMING || s_audio == A_CONNECTING) && memcmp(s_audioAddr, addr, 6) == 0;
}

void bootTick() {
    // ~3 s after boot, once: power up if an input device has been added. The
    // boot WiFi join goes first (up to 30 s): the BT firmware download takes the
    // shared radio, a join running beside it failed, and the radio left searching
    // for the network made the A2DP stream stutter (hw 2026-10-30).
    static bool done = false;
    if (done || s_state != S_OFF || nowMs() < 3000) return;
    if (WifiNet::bootJoinPending() && nowMs() < 30000) return;
    done = true;
    Known k;
    if (listLoad(true, &k, 1) >= 1) blog("added input %s - starting Bluetooth", k.name);
    else if (audioOnSaved() && listLoad(false, &k, 1) >= 1) blog("speaker %s was in use - starting Bluetooth", k.name);
    else return;
    powerOnAsync();
}

AudioState audioState() { return s_audio; }
const char* audioName() { return s_audioName; }

bool audioConnect(const uint8_t addr[6], const char* name) {
    if (!ensureUp()) return false;
    if (s_busy) finish("stopped");
    const bool switching = s_cid != 0;
    if (switching) audioDrop();
    memcpy(s_audioAddr, addr, 6);
    snprintf(s_audioName, sizeof(s_audioName), "%s", name && name[0] ? name : bd_addr_to_str(s_audioAddr));
    s_audioRetries = 0;
    if (switching) {
        // The stack holds ONE A2DP connection (MAX_NR_AVDTP_CONNECTIONS): the
        // new one may only start once the old one is gone (connecting right away
        // failed with 0x56 "not enough memory", then 0x1f). Continued in
        // A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED.
        s_audioPending = true;
        s_audio = A_CONNECTING;
        blog("switching to %s...", s_audioName);
        return true;
    }
    return audioEstablish();
}

// The previous device is let go in stages after "disconnected" (the AVDTP
// connection object, then the ACL link), and until then a new connect is refused
// with 0x56 (no free AVDTP connection) or 0x1f (HCI Create Connection rejected,
// "error 86"). Those two are retried every 500 ms for up to 8 s; anything else
// fails at once. audioDisconnect stops it.
static void audioRetryTick(btstack_timer_source_t*) {
    if (s_audio != A_CONNECTING || s_cid) return;    // cancelled, or got through
    audioEstablish();
}

bool audioEstablish() {
    uint16_t cid = 0;
    const uint8_t rc = a2dp_source_establish_stream(s_audioAddr, &cid);
    if (rc != ERROR_CODE_SUCCESS) {
        const bool busy = rc == BTSTACK_MEMORY_ALLOC_FAILED || rc == ERROR_CODE_UNSPECIFIED_ERROR;
        if (busy && s_audioRetries < 16) {
            if (s_audioRetries++ == 0) blog("waiting for the previous connection to close...");
            s_audio = A_CONNECTING;
            btstack_run_loop_set_timer_handler(&s_audioRetryT, audioRetryTick);
            btstack_run_loop_set_timer(&s_audioRetryT, 500);
            btstack_run_loop_add_timer(&s_audioRetryT);
            return true;
        }
        blog("connect failed to start, status 0x%02x", rc);
        s_audioRetries = 0;
        s_audio = A_FAILED;
        return false;
    }
    s_audioRetries = 0;
    s_cid = cid;
    s_audio = A_CONNECTING;
    blog("connecting to %s...", s_audioName);
    return true;
}

void audioDrop() {
    s_audioPending = false;
    btstack_run_loop_remove_timer(&s_audioRetryT);
    s_audioRetries = 0;
    ringOff();
    if (s_cid) a2dp_source_disconnect(s_cid);
    s_audio = A_IDLE;
}

void audioDisconnect() {
    audioDrop();
    audioOnSave(false);
}

// Controller up: reconnect the speaker that was playing when the board rebooted.
void audioResume() {
    if (s_audio != A_IDLE || !audioOnSaved()) return;
    Known k;
    if (listLoad(false, &k, 1) < 1) return;
    blog("reconnecting %s", k.name);
    audioConnect(k.addr, k.name);
}

bool remembered(uint8_t addr[6], char* name, int cap) {
    Known k;
    if (listLoad(false, &k, 1) < 1) return false;
    memcpy(addr, k.addr, 6);
    snprintf(name, cap, "%s", k.name);
    return true;
}

void forget(const uint8_t addr[6]) {
    bd_addr_t a;
    memcpy(a, addr, 6);
    if (s_cid && memcmp(a, s_audioAddr, 6) == 0) audioDisconnect();
    if (ensureUp()) gap_drop_link_key_for_bd_addr(a);   // the key lives in the stack's TLV
    listRemove(false, a, nullptr);
    Known in;
    if (listRemove(true, a, &in)) {
        if ((s_in == I_READY || s_in == I_CONNECTING) && memcmp(s_inAddr, a, 6) == 0) inputDisconnect();
        if (in.le && s_state == S_UP) {
            gap_auto_connection_stop((bd_addr_type_t)in.addrType, a);
            gap_delete_bonding((bd_addr_type_t)in.addrType, a);
        }
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
    // A busy guest (NxModPlayer: no frame-pacing wait) gets here once a frame,
    // and one packet a call is 50 a second against the 69 the stream needs (hw
    // 2026-10-30: the ring overflowed, ~11k samples a second thrown away). The
    // send slot for the next packet opens a radio poll or two after the last one
    // went out, so while a backlog is waiting: encode, poll until it is sent,
    // repeat - up to four packets in ~2.5 ms.
    audioWork();
    WifiNet::poll();
    for (int i = 0; i < 4 && s_st && s_audio == A_STREAMING; i++) {
        if (pcm_bt_w - pcm_bt_r < 1024) break;
        const uint32_t sent = s_st->pkts;
        for (int k = 0; k < 3 && s_st && s_st->pkts == sent; k++) {
            if (time_us_32() - now > 2500) return;
            audioWork();
            WifiNet::poll();
        }
        if (!s_st || s_st->pkts == sent) break;
    }
}

}  // namespace BtHost

// GS.cpp's host waits (a guest uploading to the card can hold core0 there for a
// whole frame and more): keep the A2DP stream fed. Weak-referenced by GS.cpp.
extern "C" void gs_host_wait_hook(void) { BtHost::pump(); }

namespace BtHost {

const char* stateStr() {
    switch (s_state) {
        case S_STARTING: return "starting";
        case S_UP:       return "up";
        case S_FAILED:   return "failed";
        default:         return "off";
    }
}

}  // namespace BtHost

#endif  // PICOSPECCY_WIFI
