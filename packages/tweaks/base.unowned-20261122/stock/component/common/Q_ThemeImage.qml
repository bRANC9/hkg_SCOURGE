import QtQuick 2.2

Image {
    id: rootImage
    property string themeSource: ""
    source: themeSource !== "" ? (themeSource + AppHomeModel.dayLightSuffix) : themeSource
} 
