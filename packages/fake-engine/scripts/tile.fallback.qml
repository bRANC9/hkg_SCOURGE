              // FAKE_ENGINE_PAYLOAD_260128
              // FAKE_ENGINE_TILE_V2_FALLBACK
              FakeEngineTile {
                  id: fakeEngineCornerTile
                  // The normal-mode grid only has five free cells on this unit and
                  // the five SCOURGE tiles take all of them, so firstEmptySlot(@ORD@)
                  // is null and the grid instance stays invisible (2026-11-24).
                  // Keep the app reachable in the widget page's free corner.
                  property var slot: root.firstEmptySlot(@ORD@)
                  // Shown whenever the allocator could not give us a grid cell, on
                  // whichever page is open: gating this on the widget page kept the
                  // app invisible on the app grid (2026-11-24: "fake engine meg sem
                  // jelent", although the fallback instance was injected).
                  visible: true
                  x: Math.max(0, root.width - k_itemWidth - 24)
                  y: ConstantModel.MENU_BAR_Y + 24
                  width: k_itemWidth
                  height: k_itemHeight
                  z: 6
                  onClicked: fakeEnginePage.open()
              }
