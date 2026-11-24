            // AMBIENT_PAYLOAD_260128
            AmbientPage {
                id: ambientPage
                x: 0
                y: ConstantModel.MENU_BAR_Y
                width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
                height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
                z: 2000
                onGoHome: close()
            }
