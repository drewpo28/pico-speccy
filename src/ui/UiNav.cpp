// pico-speccy — fullscreen menu navigation and main loop.
//
// Key handling is deliberately NOT the classic macros: is_back folds in VK_F1 *and*
// VK_MENU_LEFT and is_enter folds in VK_MENU_RIGHT (OSDMain.h), whereas here Right
// means "enter the right pane" and Left means "go back to the left pane".
//
// Only the synthetic VK_MENU_* keys are consumed. kbdExtraMapping (main.cpp) injects
// a VK_MENU_* twin alongside every raw arrow, and the repeat handler re-injects the
// synthetic ones, so handling both would move the cursor twice per press.

#include "OSDNewMenu.h"

#if NEW_UI

#include <stdio.h>
#include <string.h>

#include "UiNav.h"
#include "UiRender.h"
#include "UiStage.h"
#include "UiStrings.h"
#include "UiActions.h"
#include "UiDialog.h"
#include "UiBrowser.h"
#include "Subsystem.h"
#include "OSDMain.h"
#include "ESPectrum.h"
#include "Video.h"
#include "Config.h"
#include "fabutils.h"
#include "Debug.h"
#include <pico/stdlib.h>

namespace nm {

State S;

const Option opt_onoff[2] = { { "Off", 0 }, { "On", 1 } };

void DynRows::add(const char* lbl, const char* val, int32_t t, bool dimmed) {
    if (n >= NM_DYN_MAX_ROWS) return;
    snprintf(label[n], NM_DYN_LABEL_LEN, "%s", lbl ? lbl : "");
    snprintf(value[n], NM_DYN_VALUE_LEN, "%s", val ? val : "");
    tag[n] = t;
    dim[n] = dimmed ? 1 : 0;
    n++;
}

void requestClose() { S.quit = true; }

// ── level helpers ──────────────────────────────────────────────────────────────

void buildVisible(Level& L) {
    L.nvis = 0;
    if (L.dyn) {
        // The pool is already exactly what should be shown; dimmed rows stay in the list
        // (they carry information) and are refused at activation instead.
        for (uint8_t i = 0; i < S.dyn.n && L.nvis < NM_MAX_ROWS; i++) L.vis[L.nvis++] = i;
    } else {
        for (uint8_t i = 0; i < L.count && L.nvis < NM_MAX_ROWS; i++)
            if (!L.nodes[i].visible || L.nodes[i].visible())
                L.vis[L.nvis++] = i;
    }
    if (L.sel >= L.nvis) L.sel = L.nvis ? L.nvis - 1 : 0;
    if (L.top > L.sel)   L.top = L.sel;
    if (L.sel >= L.top + LY.body_rows)
        L.top = L.sel - LY.body_rows + 1;
}

// Hidden children must not appear in the right-pane preview either, so the count and
// the row lookup both walk the visible subset.
const Node* subVisibleChild(const Node* n, int i) {
    if (!n || n->kind != K_SUB) return nullptr;
    int seen = 0;
    for (uint8_t k = 0; k < n->count; k++) {
        const Node& c = n->kids[k];
        if (c.visible && !c.visible()) continue;
        if (seen == i) return &c;
        seen++;
    }
    return nullptr;
}

int rightRowCount(const Node* n) {
    if (!n) return 0;
    switch (n->kind) {
        case K_RADIO:
        case K_BOOL: { uint8_t c; nodeOptions(*n, c); return c; }
        case K_SUB: {
            int v = 0;
            for (uint8_t k = 0; k < n->count; k++)
                if (!n->kids[k].visible || n->kids[k].visible()) v++;
            return v;
        }
        default:     return 1;
    }
}

const char* rightTitle(const Node* n) {
    return n ? n->label : "";
}

void breadcrumb(char* out, size_t cap) {
    // "Settings > Storage > Betadisk" — walks the parent chain of the open levels.
    size_t used = snprintf(out, cap, "Settings");
    for (uint8_t d = 1; d <= S.depth && used < cap; d++) {
        const Node* p = S.lv[d].parent;
        if (!p) break;
        used += snprintf(out + used, cap - used, " > %s", p->label);
    }
}

// A node the user can act on (submenus, values, actions). Dimmed dynamic rows are
// skipped by the row builder itself, so nothing else needs to filter here.
static bool isDescendable(const Node* n) {
    return n && (n->kind == K_SUB || n->kind == K_DYNAMIC);
}
static bool hasValuePane(const Node* n) {
    return n && (n->kind == K_RADIO || n->kind == K_BOOL);
}
static bool isIntNode(const Node* n) {
    return n && n->kind == K_INT;
}

// ── key decode ─────────────────────────────────────────────────────────────────

enum NmKey : uint8_t {
    NK_NONE, NK_UP, NK_DOWN, NK_LEFT, NK_RIGHT, NK_ENTER,
    NK_ESC, NK_CLOSE, NK_HOME, NK_END, NK_PGUP, NK_PGDN,
    NK_F2, NK_F3, NK_F4, NK_F6, NK_F8   // per-row verbs, only meaningful on dynamic levels
};

static NmKey decode(const fabgl::VirtualKeyItem& k) {
    switch (k.vk) {
        case fabgl::VK_MENU_UP:    return NK_UP;
        case fabgl::VK_MENU_DOWN:  return NK_DOWN;
        case fabgl::VK_MENU_LEFT:  return NK_LEFT;
        case fabgl::VK_MENU_RIGHT: return NK_RIGHT;
        case fabgl::VK_MENU_ENTER: return NK_ENTER;
        case fabgl::VK_MENU_HOME:
        case fabgl::VK_HOME:       return NK_HOME;
        case fabgl::VK_END:        return NK_END;
        case fabgl::VK_PAGEUP:     return NK_PGUP;
        case fabgl::VK_PAGEDOWN:   return NK_PGDN;
        case fabgl::VK_F2:         return NK_F2;
        case fabgl::VK_F3:         return NK_F3;
        case fabgl::VK_F4:         return NK_F4;
        case fabgl::VK_F6:         return NK_F6;
        case fabgl::VK_F8:         return NK_F8;
        case fabgl::VK_ESCAPE:     return NK_ESC;
        case fabgl::VK_F1:         return NK_CLOSE;   // closes from any depth
        default:                   return NK_NONE;
    }
}

// ── movement ───────────────────────────────────────────────────────────────────

// What the right pane shows for a dynamic row: the verbs available on it.
static const char* const kDynHints[] = {
    SYM_ENTER " Mount a file",
    "F2  Write protect",
    "F8  Eject",
};

const char* dynHint(int i) {
    // A dynamic level may carry its own verb list (NM_DYNH puts it in opts).
    const Node* o = dynOwner();
    if (o && o->opts && o->count) {
        return (i >= 0 && i < (int)o->count) ? o->opts[i].label : nullptr;
    }
    const int n = (int)(sizeof(kDynHints) / sizeof(kDynHints[0]));
    return (i >= 0 && i < n) ? kDynHints[i] : nullptr;
}

static void refreshRightPane() {
    if (curLevel().dyn) {
        const Node* o = dynOwner();
        S.rcount = (o && o->opts && o->count)
                       ? o->count
                       : (uint8_t)(sizeof(kDynHints) / sizeof(kDynHints[0]));
        S.rtop = S.rsel = 0;
        return;
    }
    const Node* n = curNode();
    if (n && !nodeEnabled(*n)) {           // greyed row: nothing to interact with
        S.rcount = 0;
        S.rtop = S.rsel = 0;
        return;
    }
    S.rcount = (uint8_t)rightRowCount(n);
    S.rtop = 0;
    S.rsel = 0;
    if (hasValuePane(n)) {                 // land on the currently selected option
        int32_t v = nodeValue(*n);
        uint8_t cnt; const Option* o = nodeOptions(*n, cnt);
        for (uint8_t i = 0; i < cnt; i++)
            if (o[i].value == v) { S.rsel = i; break; }
        if (S.rsel >= LY.body_rows) S.rtop = S.rsel - LY.body_rows + 1;
    }
}

static void moveLeft(int delta) {
    Level& L = curLevel();
    if (!L.nvis) return;
    int old = L.sel, oldTop = L.top;
    int s = L.sel + delta;
    if (s < 0) s = 0;
    if (s > L.nvis - 1) s = L.nvis - 1;
    L.sel = (uint8_t)s;
    if (L.sel < L.top) L.top = L.sel;
    if (L.sel >= L.top + LY.body_rows) L.top = L.sel - LY.body_rows + 1;

    if (L.sel == old && L.top == oldTop) return;
    if (L.top != oldTop) markDirty(D_LEFT);
    else { markLeftRow(old - L.top); markLeftRow(L.sel - L.top); }
    refreshRightPane();
    markDirty(D_PTITLE | D_RIGHT | D_FOOT);
}

static void moveRight(int delta) {
    const Node* n = curNode();
    if (!hasValuePane(n) || !S.rcount) return;
    int old = S.rsel, oldTop = S.rtop;
    int s = S.rsel + delta;
    if (s < 0) s = 0;
    if (s > S.rcount - 1) s = S.rcount - 1;
    S.rsel = (uint8_t)s;
    if (S.rsel < S.rtop) S.rtop = S.rsel;
    if (S.rsel >= S.rtop + LY.body_rows) S.rtop = S.rsel - LY.body_rows + 1;

    if (S.rsel == old && S.rtop == oldTop) return;
    if (S.rtop != oldTop) markDirty(D_RIGHT);
    else { markRightRow(old - S.rtop); markRightRow(S.rsel - S.rtop); }
    markDirty(D_FOOT);
}

// Re-evaluate visible() for the open level without losing the cursor: rows can appear or
// disappear underneath it, so the NODE index is what gets restored, not the row number.
static void rebuildKeepingSelection() {
    Level& L = curLevel();
    const uint8_t wanted = L.nvis ? L.vis[L.sel] : 0;
    buildVisible(L);
    for (uint8_t i = 0; i < L.nvis; i++)
        if (L.vis[i] == wanted) { L.sel = i; break; }
    if (L.sel < L.top) L.top = L.sel;
    if (L.sel >= L.top + LY.body_rows) L.top = L.sel - LY.body_rows + 1;
}

static void enterLevel(const Node* n) {
    if (S.depth + 1 >= NM_MAX_DEPTH) return;
    S.depth++;
    Level& L = curLevel();
    L.parent = n;
    L.sel = L.top = 0;
    L.dyn = false;
    if (n->kind == K_SUB) {
        L.nodes = n->kids;
        L.count = n->count;
    } else if (n->kind == K_DYNAMIC && n->build) {
        // Built ONCE, on entry — never per keypress, or holding Down would hammer the SD
        // card. Rows are re-built explicitly after an action that changes them.
        S.dyn.clear();
        n->build(S.dyn);
        L.nodes = nullptr;
        L.count = S.dyn.n;
        L.dyn   = true;
    } else {
        S.depth--;
        return;
    }
    buildVisible(L);
    if (L.dyn && S.dyn.focus_set) {
        // The builder asked to land on a specific row (persist picker: the last
        // used slot).
        for (uint8_t i = 0; i < L.nvis; i++)
            if (S.dyn.tag[L.vis[i]] == S.dyn.focus_tag) { L.sel = i; break; }
        if (L.sel >= L.top + LY.body_rows) L.top = L.sel - LY.body_rows + 1;
    }
    S.focus = FOCUS_LEFT;
    refreshRightPane();
    markDirty(D_SUB | D_PTITLE | D_LEFT | D_RIGHT | D_FOOT);
}

static void leaveLevel() {
    if (S.depth <= S.home_depth) { S.quit = true; return; }
    S.depth--;
    S.focus = FOCUS_LEFT;
    buildVisible(curLevel());
    refreshRightPane();
    markDirty(D_SUB | D_PTITLE | D_LEFT | D_RIGHT | D_FOOT);
}

// A reused modal dialog owns the whole screen while it runs, so the fullscreen has
// to be repainted afterwards. Two traps:
//  * fileDialog pops a SaveRect it never pushed (it relies on menuRun having pushed
//    one), so menu_saverect must be false around it;
//  * in DS80 our 16 UI colours have REPLACED the 16 Profi palette entries, and those
//    dialogs draw with zxColor() indices 0..15 — they would come out in our colours.
//    Hand the palette back for the duration of the dialog and re-install after.
static void runModal(void (*fn)()) {
    if (!fn) return;
    OSD::menu_level = 0;
    OSD::menu_saverect = false;
    gfxSuspendPalette();
    fn();
    gfxResumePalette();
    // A modal may have painted anywhere, including over the separator and the
    // window border, which flushDirty never repaints — restore the chrome first.
    drawFrameOnce();
    markDirty(D_ALL);
}

static void runModalArg(void (*fn)(int32_t), int32_t arg) {
    if (!fn) return;
    OSD::menu_level = 0;
    OSD::menu_saverect = false;
    gfxSuspendPalette();
    fn(arg);
    gfxResumePalette();
    drawFrameOnce();
    markDirty(D_ALL);
}

// Run a dynamic row's verb, then rebuild the pool: mounting or ejecting changes exactly
// the rows we are looking at.
static void dynInvoke(uint8_t key) {
    Level& L = curLevel();
    const Node* owner = L.parent;
    if (!L.dyn || !owner || !owner->rowkey || !L.nvis) return;
    const uint8_t r = L.vis[L.sel];
    if (S.dyn.dim[r]) return;                    // informational row
    const int32_t tag = S.dyn.tag[r];
    Debug::log("dynInvoke: tag=%ld key=%u sp=%08x\n", (long)tag, (unsigned)key, debug_sp());

    OSD::menu_level = 0;
    OSD::menu_saverect = false;
    gfxSuspendPalette();
    owner->rowkey(tag, key);
    gfxResumePalette();

    if (owner->build) { S.dyn.clear(); owner->build(S.dyn); }
    L.count = S.dyn.n;
    buildVisible(L);
    drawFrameOnce();
    markDirty(D_ALL);
}

static void activate() {
    if (curLevel().dyn) { dynInvoke(0); return; }
    const Node* n = curNode();
    if (!n || !nodeEnabled(*n)) return;    // greyed rows are inert
    switch (n->kind) {
        case K_SUB:
        case K_DYNAMIC:
            enterLevel(n);
            break;
        case K_RADIO:
        case K_BOOL:
            if (S.focus == FOCUS_LEFT) {           // Right/Enter: step into the pane
                S.focus = FOCUS_RIGHT;
                markLeftRow(curLevel().sel - curLevel().top);
                markDirty(D_RIGHT | D_FOOT);
            } else {                               // commit the highlighted option
                uint8_t cnt; const Option* o = nodeOptions(*n, cnt);
                if (S.rsel >= cnt) break;
                nodeSetValue(*n, o[S.rsel].value);
                // A value can gate other rows: picking Profi reveals XT keyboard and OSD
                // palette, picking a Byte ROM reveals COBMECT mode. Rebuild the visible
                // set so that happens now rather than on the next entry into the level.
                rebuildKeepingSelection();
                // An instant-apply hook may have drawn modal boxes over the menu (the
                // transport's busy/reboot dialogs) — restore the chrome like runModal.
                if (Stage::editDrawsModal(n->setting)) drawFrameOnce();
                markDirty(Stage::editDrawsModal(n->setting)
                              ? D_ALL : (D_LEFT | D_RIGHT | D_FOOT));
            }
            break;
        case K_INT:
            if (S.focus == FOCUS_LEFT) {
                S.focus = FOCUS_RIGHT;
                markLeftRow(curLevel().sel - curLevel().top);
                markDirty(D_RIGHT | D_FOOT);
            } else {                               // Enter leaves the slider
                S.focus = FOCUS_LEFT;
                markLeftRow(curLevel().sel - curLevel().top);
                markDirty(D_RIGHT | D_FOOT);
            }
            break;
        case K_ACTION:
        case K_PAGE:
            runModal(n->fn);
            break;
        case K_ACTION_ARG:
            runModalArg(n->pick, n->lo);
            break;
        default:
            break;
    }
}

// Adjust the focused K_INT by mult steps, clamped to [lo, hi]. Redraws the slider,
// the left row's value label and the pending counter.
static void stepInt(int mult) {
    const Node* n = curNode();
    if (!isIntNode(n) || n->step == 0) return;
    const int32_t old = nodeValue(*n);
    int32_t v = old + (int32_t)n->step * mult;
    if (v < n->lo) v = n->lo;
    if (v > n->hi) v = n->hi;
    if (v == old) return;
    nodeSetValue(*n, v);
    markRightRow(0);
    markLeftRow(curLevel().sel - curLevel().top);
    markDirty(D_FOOT);
}

static bool handleKey(NmKey k) {
    // In the right pane of a K_INT row, the vertical keys adjust the value.
    const bool intPane = (S.focus == FOCUS_RIGHT) && !curLevel().dyn && isIntNode(curNode());
    if (intPane) {
        switch (k) {
            case NK_UP:    stepInt(+1); return S.quit;
            case NK_DOWN:  stepInt(-1); return S.quit;
            case NK_PGUP:  stepInt(+4); return S.quit;
            case NK_PGDN:  stepInt(-4); return S.quit;
            case NK_HOME:  stepInt(+32767); return S.quit;
            case NK_END:   stepInt(-32767); return S.quit;
            default: break;                       // Enter/Left/Esc fall through below
        }
    }
    switch (k) {
        case NK_UP:    (S.focus == FOCUS_LEFT) ? moveLeft(-1) : moveRight(-1); break;
        case NK_DOWN:  (S.focus == FOCUS_LEFT) ? moveLeft(+1) : moveRight(+1); break;
        case NK_PGUP:  (S.focus == FOCUS_LEFT) ? moveLeft(-LY.body_rows)
                                               : moveRight(-LY.body_rows); break;
        case NK_PGDN:  (S.focus == FOCUS_LEFT) ? moveLeft(+LY.body_rows)
                                               : moveRight(+LY.body_rows); break;
        case NK_HOME:  (S.focus == FOCUS_LEFT) ? moveLeft(-NM_MAX_ROWS)
                                               : moveRight(-NM_MAX_ROWS); break;
        case NK_END:   (S.focus == FOCUS_LEFT) ? moveLeft(+NM_MAX_ROWS)
                                               : moveRight(+NM_MAX_ROWS); break;
        case NK_RIGHT:
        case NK_ENTER: activate(); break;
        case NK_LEFT:
            if (S.focus == FOCUS_RIGHT) {
                S.focus = FOCUS_LEFT;
                markLeftRow(curLevel().sel - curLevel().top);
                markDirty(D_RIGHT | D_FOOT);
            } else {
                leaveLevel();
            }
            break;
        case NK_ESC:
            if (S.focus == FOCUS_RIGHT) {
                S.focus = FOCUS_LEFT;
                markLeftRow(curLevel().sel - curLevel().top);
                markDirty(D_RIGHT | D_FOOT);
            } else {
                leaveLevel();
            }
            break;
        case NK_F2: if (curLevel().dyn) dynInvoke(2); break;
        // F3/F4 mirror the classic persist dialogs, where the opening key repeated
        // acts as Enter. Handlers that don't know them ignore them.
        case NK_F3: if (curLevel().dyn) dynInvoke(3); break;
        case NK_F4: if (curLevel().dyn) dynInvoke(4); break;
        case NK_F6: if (curLevel().dyn) dynInvoke(6); break;
        case NK_F8: if (curLevel().dyn) dynInvoke(8); break;
        case NK_CLOSE: S.quit = true; break;
        default: break;
    }
    return S.quit;
}

// ── public entry points ────────────────────────────────────────────────────────

bool available() {
    gfxComputeSurface();          // no palette side effects
    computeLayout();
    return layoutFits();
}

// Depth-first search for `target`, recording the K_SUB chain that reaches it. Used to
// open the menu already positioned on a level (the disk hot key).
static bool findPath(const Node* nodes, uint8_t count, const Node* target,
                     const Node** chain, int depth, int cap, int* outLen) {
    for (uint8_t i = 0; i < count; i++) {
        const Node& n = nodes[i];
        if (&n == target) {
            if (depth >= cap) return false;
            chain[depth] = &n;
            *outLen = depth + 1;
            return true;
        }
        if (n.kind == K_SUB && depth < cap) {
            chain[depth] = &n;
            if (findPath(n.kids, n.count, target, chain, depth + 1, cap, outLen)) return true;
        }
    }
    return false;
}

static void openPath(const Node* target) {
    const Node* chain[NM_MAX_DEPTH];
    int len = 0;
    if (!findPath(rootNodes(), rootNodeCount(), target, chain, 0, NM_MAX_DEPTH, &len)) return;
    for (int d = 0; d < len; d++) {
        // Land the cursor on the node first so the level we push has the right parent and
        // the breadcrumb reads correctly.
        Level& L = curLevel();
        for (uint8_t i = 0; i < L.nvis; i++)
            if (&L.nodes[L.vis[i]] == chain[d]) { L.sel = i; break; }
        if (L.sel >= L.top + LY.body_rows) L.top = L.sel - LY.body_rows + 1;
        if (chain[d]->kind == K_SUB || chain[d]->kind == K_DYNAMIC) enterLevel(chain[d]);
    }
}

static void runInternal(const Node* openAt) {
    Debug::log("runInternal: sp=%08x\n", debug_sp());
    gfxBegin();               // installs the UI palette (own 16 colours)
    computeLayout();
    if (!layoutFits()) { gfxEnd(); return; }

    // Classic text pages (ChipInfo, ...) render in the new style while we are up,
    // and long operations (Speed test, ZIP extract, DLS convert) report in the
    // footer status line instead of the classic centered box.
    OSD::textPageOverride = uiTextPage;
    OSD::progressOverride = uiProgressStatus;

    // The fullscreen is never pushed onto the SaveRect stack (320x240 = 77 KB per
    // open). The emulated frame is repainted by ESPectrum::processKeyboard on close.
    VIDEO::SaveRect.clear();
    OSD::menu_level = 0;
    OSD::menu_saverect = false;

    memset(&S, 0, sizeof(S));
    Stage::begin();
    netStatusInvalidate();      // WiFi state may have changed since the last session
    S.depth = 0;
    S.focus = FOCUS_LEFT;
    Level& L = curLevel();
    L.nodes = rootNodes();
    L.count = rootNodeCount();
    L.parent = nullptr;
    buildVisible(L);
    refreshRightPane();

    if (openAt) { openPath(openAt); S.home_depth = S.depth; }

    drawFrameOnce();
    markDirty(D_ALL);
    flushDirty();

resume:
    while (!S.quit) {
        bool acted = false;
        while (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            fabgl::VirtualKeyItem k;
            if (!ESPectrum::readKbd(&k)) continue;
            if (!k.down) continue;
            NmKey nk = decode(k);
            if (nk == NK_NONE) continue;
            acted = true;
            if (handleKey(nk)) break;
        }
        if (acted) {
            if (menuMarqueeActive())
                markLeftRow(curLevel().sel - curLevel().top);
            menuMarqueeReset();
            OSD::clickNoPause();
            flushDirty();
        }
        if (!S.quit) {
            sleep_ms(5);
            if (menuMarqueeTick()) {
                markLeftRow(curLevel().sel - curLevel().top);
                flushDirty();
            }
            if (uiClockDirty()) {
                markDirty(D_HEADER);
                flushDirty();
            }
        }
    }

    // ── the single exit point ──────────────────────────────────────────────────
    // Everything staged is applied here, in one place. This is what do_OSD cannot do:
    // it leaves through 47 different returns.
    //
    // With anything staged the exit always asks: Apply (labelled with the reboot
    // when one is coming), No (drop every edit, undoing live previews), or
    // Cancel (back into the menu with the edits intact).
    bool doCommit = true;
    while (Stage::anyDirty()) {
        const bool reboot = Stage::rebootPending();
        const char* btns[3] = { reboot ? "Apply+Reboot" : "Apply", "No", "Cancel" };
        char q[48];
        snprintf(q, sizeof(q), "Apply %u changed setting%s?",
                 (unsigned)Stage::dirtyCount(), Stage::dirtyCount() == 1 ? "" : "s");
        const int c = uiChoice(q, btns, 3, 0, /*esc=*/2);
        if (c == 2) {                          // Cancel: back into the menu
            S.quit = false;
            drawFrameOnce();
            markDirty(D_ALL);
            flushDirty();
            goto resume;
        }
        if (c == 1) {                          // No: drop the staged edits
            Stage::discard();
            doCommit = false;
        }
        break;                                 // Apply (or No) → leave the menu
    }

    Stage::CommitReport rep = {};
    if (doCommit) Stage::commit(rep);

    // At most ONE message, and at most ONE question, for the whole session.
    // Not after a machine switch though: the surface computed at open may be gone
    // (DS80 -> standard), so drawing with it would garble — and the switch spoke
    // for itself (MachineSwitch shows its own messages).
    if (!rep.machineSwitched) {
        if (rep.blocked) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Not enough free SRAM for %s",
                     Subsystems::featureName((Subsystems::FeatureId)rep.blockedFeat));
            uiToast(msg, true, 2000);
        } else if (rep.note) {
            uiToast(rep.note, true, 2000);
        } else if (rep.failed) {
            uiToast("Some settings could not be applied", true, 1500);
        }
    }

    // A declined machine switch needs no message of ours: featureBudgetGate already
    // explained itself, and MachineSwitch left the machine untouched.
    // The "Apply+Reboot" button already carried the consent — reboot right away.
    if (rep.needsReboot)
        OSD::esp_hard_reset();              // never returns

    // MUST run on every exit path: in DS80 our 16 colours are installed in the Profi
    // palette, so skipping this leaves the guest's screen painted in UI colours.
    OSD::textPageOverride = nullptr;
    OSD::progressOverride = nullptr;
    gfxEnd();
}

void run() { runInternal(nullptr); }

void runDiskSlots(int iface, const char* fname) {
    Debug::log("runDiskSlots: iface=%d file='%s' sp=%08x\n", iface, fname ? fname : "", debug_sp());
    // The fullscreen layout may not fit the live video mode; the call sites in
    // OSDMain run Config::save()/reset() right after us, so silently mounting
    // nothing is not an option — fall back to the classic slot popup.
    if (!available()) {
        OSD::diskSlotDialog((DiskIface)iface, 0, fname ? fname : "");
        return;
    }
    const Node* target = slotNodeFor(iface);
    if (!target) return;
    slotsArmFile(fname ? fname : "");
    runInternal(target);
    slotsArmFile("");           // never let a stale pre-armed file survive the session
}

void runPersist(bool save) {
    if (!available()) return;   // call sites fall back to the classic dialogs
    const Node* target = persistNodeFor(save);
    if (target) runInternal(target);
}

} // namespace nm

#endif // NEW_UI
