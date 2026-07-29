// pico-speccy — declarative model for the new fullscreen OSD menu.
//
// The tree is a static, const, flash-resident array of Node. There is no runtime
// construction and no heap: RAM is scarce (~95-107 KB free heap at runtime) and a
// node array that needs a runtime initialiser would silently move from .rodata to
// .data.
//
// The one rule that matters: option values are stored as explicit {label, value}
// pairs, NEVER positionally. The classic menu encodes the stored enum in the row
// order (`Config::x = opt2 - 1`, ~200 sites, plus marker lookup by literal such as
// `menu.find("[B")`), which is why no row there can be reordered or renamed without
// silently changing the meaning of a saved setting. Here display order and stored
// value are independent by construction.
//
// Conditional rows use a `visible()` predicate rather than rebuilding the row list.
// That removes the whole class of index arithmetic the classic menu needs
// (`gs_avail ? 9 : 8`, `(mos && opt2 == 5) || (!mos && opt2 == 4)`, ...).

#pragma once

#if NEW_UI

#include <stdint.h>
#include <stddef.h>

namespace nm {

enum Kind : uint8_t {
    K_SUB,      // static child list (kids/count)
    K_DYNAMIC,  // child list produced by build() when the level is entered
    K_RADIO,    // one-of-N over opts[]; stored value is Option::value
    K_BOOL,     // Yes/No; sugar over a 2-entry opts[]
    K_INT,      // integer range, Left/Right steps by `step`
    K_TEXT,     // inline text field, edited in place via OSD::inlineTextEdit
    K_ACTION,   // runs fn() immediately (not a setting — never staged)
    K_ACTION_ARG, // like K_ACTION but the node carries the argument, so eight ROM slots
                  //   are eight rows over ONE function instead of eight wrappers — and the
                  //   slot index sits in the row, not in its position
    K_PAGE,     // full-screen info page; fn() owns its own key loop
};

struct Option {
    const char* label;
    int32_t     value;      // the value actually stored in Config — not the row index
    // Optional compact form for the left-column value ("Auto" vs the right pane's
    // "Auto (Profi only)"); nullptr (the default) = label is used in both places.
    const char* slabel;
};

// Rows of a K_DYNAMIC level are built into this fixed pool. Labels are COPIED, so a
// builder can format from a std::string without leaving a dangling pointer behind.
#define NM_DYN_MAX_ROWS   40
#define NM_DYN_LABEL_LEN  48
#define NM_DYN_VALUE_LEN  22

struct DynRows {
    uint8_t n;
    char    label[NM_DYN_MAX_ROWS][NM_DYN_LABEL_LEN];
    char    value[NM_DYN_MAX_ROWS][NM_DYN_VALUE_LEN];
    int32_t tag[NM_DYN_MAX_ROWS];       // passed to the level's action as its argument
    uint8_t dim[NM_DYN_MAX_ROWS];       // 1 = shown greyed out and not selectable
    // Optional short flag drawn immediately after the label, in its own colour:
    // state 1 = active (red, "this is on"), 2 = inactive (greyed). Disk slots use it
    // for write-protect, which as a value-column suffix was invisible the moment the
    // filename needed truncating — which is most of the time.
    char    badge[NM_DYN_MAX_ROWS][5];
    uint8_t badge_st[NM_DYN_MAX_ROWS];  // 0 = no badge
    // A builder may ask the nav to land the cursor on the row with this tag
    // (e.g. the persist picker opens on Config::persist_slot).
    int32_t focus_tag;
    bool    focus_set;

    void clear() { n = 0; focus_set = false; }
    void add(const char* lbl, const char* val = nullptr, int32_t t = 0, bool dimmed = false);
    // Flags the row just added (no-op if none was).
    void badgeLast(const char* txt, bool active);
    void focusTag(int32_t t) { focus_tag = t; focus_set = true; }
};

struct Node {
    const char*   label;
    Kind          kind;
    uint8_t       count;        // opts[] entries (K_RADIO/K_BOOL) or kids[] entries (K_SUB)
    uint16_t      setting;      // SettingId; 0 = not a stored setting
    const Option* opts;
    const Node*   kids;
    bool        (*visible)();               // nullptr = always visible
    void        (*fn)();                    // K_ACTION / K_PAGE
    void        (*build)(DynRows&);         // K_DYNAMIC: fill the row pool
    void        (*pick)(int32_t tag);       // K_ACTION_ARG: the argument-taking action
    // K_DYNAMIC: a row was activated. `key` is 0 for Enter, else the function-key number
    // (2 = F2, 8 = F8), so one callback covers "open" and the per-row verbs a slot editor
    // needs without the nav knowing what they mean.
    void        (*rowkey)(int32_t tag, uint8_t key);
    int16_t       lo, hi, step;             // K_INT
    // Optional dynamic value label for the row (drawn right-aligned like a stored
    // value). Lets an action row show live state ("WiFi: On <ssid>") without being
    // a stored setting. nullptr = no dynamic label.
    const char* (*vlabel)();
    // Optional enable gate, independent of visible(): a row whose gate returns
    // false stays VISIBLE but greyed out and inert — the generic "these options
    // belong to a feature that is currently off" affordance (Betadisk children,
    // and reusable for anything else). nullptr = always enabled.
    bool        (*enabled)();
    // K_RADIO only: runtime option-table provider (NM_RADIO_D). When set, opts/count
    // are ignored and every reader goes through nodeOptions(). The table must be
    // stable for the menu session (per-board lists built once).
    const Option* (*dopts)(uint8_t& cnt);
};

// Counts are derived from the array, never written by hand: a hand-written count
// that drifts from the array is the one bug this model cannot otherwise catch.
#define NM_COUNT(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

#define NM_SUB(lbl, kids_arr, vis) \
    { lbl, nm::K_SUB, NM_COUNT(kids_arr), 0, nullptr, kids_arr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr }
#define NM_ACTION(lbl, f, vis) \
    { lbl, nm::K_ACTION, 0, 0, nullptr, nullptr, vis, f, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr }
// Action whose row shows live state via `vl` (e.g. "On <ssid>").
#define NM_ACTIONV(lbl, f, vl, vis) \
    { lbl, nm::K_ACTION, 0, 0, nullptr, nullptr, vis, f, nullptr, nullptr, nullptr, 0, 0, 0, vl, nullptr }
#define NM_ACTION_EN(lbl, f, vis, en) \
    { lbl, nm::K_ACTION, 0, 0, nullptr, nullptr, vis, f, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, en }
#define NM_ACTIONV_EN(lbl, f, vl, vis, en) \
    { lbl, nm::K_ACTION, 0, 0, nullptr, nullptr, vis, f, nullptr, nullptr, nullptr, 0, 0, 0, vl, en }
// Reuses `pick` (the K_DYNAMIC row callback) as the argument-taking action and `lo` as the
// argument: both are unused for actions, so Node stays the same size.
#define NM_ACTION_ARG(lbl, f, arg, vis) \
    { lbl, nm::K_ACTION_ARG, 0, 0, nullptr, nullptr, vis, nullptr, nullptr, f, nullptr, arg, 0, 0, nullptr, nullptr }
#define NM_PAGE(lbl, f, vis) \
    { lbl, nm::K_PAGE, 0, 0, nullptr, nullptr, vis, f, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr }
#define NM_RADIO(lbl, sid, opts_arr, vis) \
    { lbl, nm::K_RADIO, NM_COUNT(opts_arr), sid, opts_arr, nullptr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr }
// Enable-gated variant: visible always, greyed and inert while en() is false.
#define NM_RADIO_EN(lbl, sid, opts_arr, vis, en) \
    { lbl, nm::K_RADIO, NM_COUNT(opts_arr), sid, opts_arr, nullptr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, en }
#define NM_BOOL(lbl, sid, vis) \
    { lbl, nm::K_BOOL, NM_COUNT(nm::opt_onoff), sid, nm::opt_onoff, nullptr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr }
#define NM_BOOL_EN(lbl, sid, vis, en) \
    { lbl, nm::K_BOOL, NM_COUNT(nm::opt_onoff), sid, nm::opt_onoff, nullptr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, en }
#define NM_INT(lbl, sid, l, h, s, vis) \
    { lbl, nm::K_INT, 0, sid, nullptr, nullptr, vis, nullptr, nullptr, nullptr, nullptr, l, h, s, nullptr, nullptr }
#define NM_DYN(lbl, bld, rk, vis) \
    { lbl, nm::K_DYNAMIC, 0, 0, nullptr, nullptr, vis, nullptr, bld, nullptr, rk, 0, 0, 0, nullptr, nullptr }
#define NM_DYN_EN(lbl, bld, rk, vis, en) \
    { lbl, nm::K_DYNAMIC, 0, 0, nullptr, nullptr, vis, nullptr, bld, nullptr, rk, 0, 0, 0, nullptr, en }
// Dynamic level with its own right-pane verb list (an Option[] whose labels are the
// hints) instead of the default Mount/WP/Eject set of the disk slots.
#define NM_DYNH(lbl, bld, rk, hints, vis) \
    { lbl, nm::K_DYNAMIC, NM_COUNT(hints), 0, hints, nullptr, vis, nullptr, bld, nullptr, rk, 0, 0, 0, nullptr, nullptr }
#define NM_DYNH_EN(lbl, bld, rk, hints, vis, en) \
    { lbl, nm::K_DYNAMIC, NM_COUNT(hints), 0, hints, nullptr, vis, nullptr, bld, nullptr, rk, 0, 0, 0, nullptr, en }
// Radio whose option table is built at RUNTIME (`df`): per-board lists like the ESP
// transport's GPIO pairs, which no static array can spell out. Behaves exactly like
// NM_RADIO otherwise — same right-pane rings, same staged value.
#define NM_RADIO_D(lbl, sid, df, vis) \
    { lbl, nm::K_RADIO, 0, sid, nullptr, nullptr, vis, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, nullptr, nullptr, df }

extern const Option opt_onoff[2];

// The tree root, defined in UiTree.cpp.
const Node* rootNodes();
const Node* slotNodeFor(int iface);   // DiskIface -> its K_DYNAMIC slot level
const Node* persistNodeFor(bool save);// the Save/Load-snapshot K_DYNAMIC level
uint8_t     rootNodeCount();

// True when the node's enable gate (if any) allows interaction.
inline bool nodeEnabled(const Node& n) { return !n.enabled || n.enabled(); }

// Option table of a K_RADIO/K_BOOL node: the static opts[] normally, or the
// runtime-built table when the node carries a dopts() provider.
inline const Option* nodeOptions(const Node& n, uint8_t& cnt) {
    if (n.dopts) return n.dopts(cnt);
    cnt = n.count;
    return n.opts;
}

// Current value of a value-carrying node, and the label to show for it.
// Backed by the staging overlay (UiStage), so what the menu shows is the staged
// value when the user has touched it and the live value otherwise.
int32_t     nodeValue(const Node& n);
void        nodeSetValue(const Node& n, int32_t v);
const char* nodeValueLabel(const Node& n);   // "" when the node carries no value

} // namespace nm

#endif // NEW_UI
