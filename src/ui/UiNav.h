// pico-speccy — fullscreen menu navigation state (internal to src/ui/).
#pragma once

#if NEW_UI

#include <stdint.h>
#include "UiModel.h"

namespace nm {

enum Focus : uint8_t { FOCUS_LEFT = 0, FOCUS_RIGHT = 1 };

#define NM_MAX_DEPTH 5
#define NM_MAX_ROWS  48

// One menu level. `vis[]` maps a visible row index to a node index, so a hidden row
// (visible() == false) never shifts the identity of the rows around it.
struct Level {
    const Node* nodes;
    const Node* parent;         // nullptr at the root
    uint8_t     count;
    uint8_t     sel;            // selected visible row
    uint8_t     top;            // first visible row on screen
    uint8_t     nvis;
    uint8_t     vis[NM_MAX_ROWS];
    bool        dyn;            // rows come from S.dyn, not from nodes[]
};

struct State {
    Level   lv[NM_MAX_DEPTH];
    uint8_t depth;              // 0 = root
    uint8_t home_depth;         // depth the session opened at (openPath) — backing out
                                // of it closes the menu, like the root does. 0 normally;
                                // the slot/persist entry points open a deeper level and
                                // must return to their caller (F5 browser), not expose
                                // the parent menu levels.
    uint8_t focus;              // FOCUS_LEFT / FOCUS_RIGHT
    uint8_t rsel, rtop, rcount; // right pane selection / scroll / row count
    bool    quit;
    DynRows dyn;                // the single dynamic-row pool (levels do not nest)
};
extern State S;

inline Level&      curLevel()      { return S.lv[S.depth]; }
// nullptr on a dynamic level: its rows are DynRows entries, not Nodes. Callers that draw
// or act on the selection must check curLevel().dyn first.
inline const Node* curNode()       { Level& L = curLevel();
                                     return (!L.dyn && L.nvis) ? &L.nodes[L.vis[L.sel]] : nullptr; }
inline const Node* dynOwner()      { Level& L = curLevel();
                                     return L.dyn ? L.parent : nullptr; }

// An action that restarted the machine (or launched something) asks the menu to close.
void        requestClose();

void        buildVisible(Level& L);
const Node* subVisibleChild(const Node* n, int i);  // i-th VISIBLE child of a K_SUB
int         rightRowCount(const Node* n);   // rows the right pane will show for `n`
const char* rightTitle(const Node* n);
void        breadcrumb(char* out, size_t n);
const char* dynHint(int i);          // right-pane verb list of a dynamic level

} // namespace nm

#endif // NEW_UI
