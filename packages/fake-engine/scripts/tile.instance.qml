              // FAKE_ENGINE_PAYLOAD_260128
              // FAKE_ENGINE_TILE_V2: one instance per grid page, same allocator as
              // the battery/wifi/tweaks tiles (root.firstEmptySlot).
              FakeEngineTile {
                  id: fakeEngineTile@PAGE@
                  // The old block gated on a list index that does not exist in the
                  // live home screen: the binding threw and the tile never appeared
                  // (2026-11-23).
                  property var slot: root.firstEmptySlot(@ORD@)
                  visible: slot !== null && slot !== undefined && slot.page === @PAGE0@
                  // Kept inside the panel: the grid is wider than the screen on this
                  // SKU, so the last column would be cut off (2026-11-24).
                  property int cellX: @MENU@.x + (slot !== null && slot !== undefined && slot.page === @PAGE0@ ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                  property int cellY: @MENU@.y + (slot !== null && slot !== undefined && slot.page === @PAGE0@ ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                  x: Math.max(0, Math.min(cellX, root.width - k_itemWidth))
                  y: Math.max(0, Math.min(cellY, root.height - k_itemHeight))
                  width: k_itemWidth
                  height: k_itemHeight
                  z: 7
                  onClicked: fakeEnginePage.open()
              }
