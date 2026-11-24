             // AMBIENT_PAYLOAD_260128
             // AMBIENT_TILE_V2: one instance per grid page, same allocator as the
             // battery/wifi/tweaks tiles (root.firstEmptySlot).
             AmbientTile {
                 id: ambientTile@PAGE@
                 // The old block gated visibility on a list index and on "index";
                 // neither name exists in this file, so the binding threw and the
                 // tile either hung off the screen edge or never appeared at all
                 // (2026-11-23).  firstEmptySlot scans every normal-mode page and
                 // returns {page, index}, so this instance shows only on the page
                 // whose grid actually got the free cell.
                 property var slot: root.firstEmptySlot(@ORD@)
                 visible: slot !== null && slot !== undefined && slot.page === @PAGE0@
                 // The grid is wider than the panel on this SKU (six columns of
                 // k_itemWidth do not fit the screen), so the last column hung off the
                 // right edge and lost its label (2026-11-24: "Engine sound").  The
                 // cell stays where the allocator put it unless that leaves the panel.
                 property int cellX: @MENU@.x + (slot !== null && slot !== undefined && slot.page === @PAGE0@ ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                 property int cellY: @MENU@.y + (slot !== null && slot !== undefined && slot.page === @PAGE0@ ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                 x: Math.max(0, Math.min(cellX, root.width - k_itemWidth))
                 y: Math.max(0, Math.min(cellY, root.height - k_itemHeight))
                 width: k_itemWidth
                 height: k_itemHeight
                 z: 7
                 onClicked: ambientPage.open()
             }
