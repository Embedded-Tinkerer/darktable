import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1440
    height: 900
    visible: true
    title: lab.hasImage ? lab.imageName + " - PhotographyLAB" : "PhotographyLAB"
    color: Theme.window

    Material.theme: Material.Dark
    Material.accent: Theme.accent
    Material.background: Theme.panel

    // unfolded or desktop: side panel. folded phone, portrait: panel below
    readonly property bool wide: width > height * 1.05 && width > 720

    function adj(key) { return lab.adjust[key] }

    header: ToolBar {
        Material.background: Theme.panel
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.gutter
            anchors.rightMargin: 8
            spacing: 4
            Text {
                text: "PhotographyLAB"
                color: Theme.text
                font.pixelSize: 18
                font.bold: true
                visible: win.wide
            }
            TabBar {
                id: modeBar
                Material.background: Theme.panel
                currentIndex: lab.mode === "library" ? 0 : 1
                TabButton { text: "Library"; width: implicitWidth; onClicked: lab.mode = "library" }
                TabButton {
                    text: "Develop"
                    width: implicitWidth
                    enabled: lab.hasImage
                    onClicked: lab.mode = "develop"
                }
            }
            Text {
                text: lab.hasImage && win.wide ? "  " + lab.imageName : ""
                color: Theme.textDim
                font.pixelSize: 14
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }
            ToolButton {
                text: "Copy edit"
                visible: lab.mode === "develop"
                onClicked: lab.copyEdit()
                ToolTip.visible: hovered
                ToolTip.text: "copy this photo's sliders and LUTs, to paste onto others in the Library"
            }
            ToolButton {
                text: "Before"
                visible: lab.mode === "develop"
                enabled: lab.hasImage
                onPressed: lab.showOriginal(true)
                onReleased: lab.showOriginal(false)
                onCanceled: lab.showOriginal(false)
                ToolTip.visible: hovered
                ToolTip.text: "hold to see the photo without your edits"
            }
            ToolButton { text: "Open"; onClicked: openDialog.open() }
            ToolButton {
                text: "Export"
                visible: lab.mode === "develop"
                enabled: lab.hasImage
                onClicked: {
                    // the photo's own folder and name, as .jpg
                    const url = lab.exportSuggestion.toString()
                    exportDialog.currentFolder = url.substring(0, url.lastIndexOf("/"))
                    exportDialog.selectedFile = lab.exportSuggestion
                    exportDialog.open()
                }
            }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: lab.mode === "library" ? 0 : 1

    LibraryView {
        wide: win.wide
    }

    GridLayout {
        columns: win.wide ? 2 : 1
        rowSpacing: 0
        columnSpacing: 0

        // ---------------------------------------------------------------- photo
        Rectangle {
            id: stage
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: win.wide ? -1 : win.height * 0.5
            color: Theme.window
            clip: true

            onWidthChanged: sizeTimer.restart()
            onHeightChanged: sizeTimer.restart()
            Timer {
                id: sizeTimer
                interval: 250
                onTriggered: lab.setViewSize(stage.width * Screen.devicePixelRatio,
                                             stage.height * Screen.devicePixelRatio)
            }
            Component.onCompleted: sizeTimer.start()

            ListView {
                id: filmstrip
                visible: win.wide && lab.images.length > 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: visible ? 96 : 0
                orientation: ListView.Horizontal
                clip: true
                spacing: 4
                model: lab.images
                currentIndex: {
                    for(let i = 0; i < lab.images.length; i++)
                        if(lab.images[i].id === lab.currentImage) return i
                    return -1
                }
                onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)
                delegate: Rectangle {
                    required property var modelData
                    width: 120
                    height: filmstrip.height
                    color: modelData.id === lab.currentImage ? Theme.panelRaised : Theme.window
                    border.width: modelData.id === lab.currentImage ? 2 : 0
                    border.color: Theme.accent
                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        cache: false
                        sourceSize.width: 240
                        sourceSize.height: 240
                        source: "image://thumb/" + parent.modelData.id + "/"
                                + (lab.thumbVersions[String(parent.modelData.id)] || 0)
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: lab.openImage(parent.modelData.id)
                    }
                }
            }

            Image {
                id: photo
                anchors.fill: parent
                anchors.margins: 12
                anchors.bottomMargin: 12 + filmstrip.height
                source: lab.previewSource
                fillMode: Image.PreserveAspectFit
                cache: false
                asynchronous: false
                smooth: true
                mipmap: true
            }

            BusyIndicator {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 16
                width: 36
                height: 36
                running: lab.busy
            }

            ColumnLayout {
                anchors.centerIn: parent
                visible: !lab.hasImage
                spacing: 12
                Text {
                    text: lab.ready ? "Open a photo to start editing" : "Starting the darktable engine..."
                    color: Theme.text
                    font.pixelSize: 20
                    Layout.alignment: Qt.AlignHCenter
                }
                Text {
                    text: "raw files from your camera, or JPEG, TIFF, PNG. You can also drop a file here."
                    color: Theme.textDim
                    font.pixelSize: 14
                    Layout.alignment: Qt.AlignHCenter
                }
                Button {
                    text: "Open photo"
                    highlighted: true
                    enabled: lab.ready
                    Layout.alignment: Qt.AlignHCenter
                    onClicked: openDialog.open()
                }
            }

            DropArea {
                anchors.fill: parent
                onDropped: (drop) => { if(drop.hasUrls) lab.openFile(drop.urls[0]) }
            }

            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                text: lab.message
                visible: text.length > 0
                color: Theme.text
                background: Rectangle { color: "#cc000000"; radius: 4 }
                padding: 8
            }
        }

        // ---------------------------------------------------------------- panel
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: !win.wide
            Layout.preferredWidth: win.wide ? 360 : -1
            Layout.preferredHeight: win.wide ? -1 : win.height * 0.42
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    currentIndex: startTab
                    Material.background: Theme.panel
                    TabButton { text: "Basic" }
                    TabButton { text: "Looks" }
                    TabButton { text: "Detail" }
                    TabButton { text: "Optics" }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabs.currentIndex
                    enabled: lab.hasImage

                    // ---------------------------------------------------- basic
                    ScrollView {
                        id: basicScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: basicScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4

                            SectionHeader { title: "White balance" }
                            AdjustSlider {
                                label: "Temperature"
                                from: 2000; to: 12000; stepSize: 50
                                unit: " K"
                                value: adj("temperature") > 0 ? adj("temperature") : lab.asShotTemperature
                                neutral: lab.asShotTemperature
                                gradient: ["#3d7bd9", "#e0b341"]
                                onMoved: (v) => lab.setValue("temperature", Math.abs(v - lab.asShotTemperature) < 25 ? 0 : v)
                            }
                            AdjustSlider {
                                label: "Tint"
                                from: -100; to: 100
                                value: adj("tint")
                                gradient: ["#4fae55", "#c04fbf"]
                                onMoved: (v) => lab.setValue("tint", v)
                            }

                            SectionHeader { title: "Tone" }
                            AdjustSlider {
                                label: "Exposure"
                                from: -5; to: 5; stepSize: 0.05; decimals: 2; unit: " EV"
                                value: adj("exposure")
                                onMoved: (v) => lab.setValue("exposure", v)
                            }
                            AdjustSlider { label: "Contrast"; value: adj("contrast"); onMoved: (v) => lab.setValue("contrast", v) }
                            AdjustSlider { label: "Highlights"; value: adj("highlights"); onMoved: (v) => lab.setValue("highlights", v) }
                            AdjustSlider { label: "Shadows"; value: adj("shadows"); onMoved: (v) => lab.setValue("shadows", v) }
                            AdjustSlider { label: "Blacks"; value: adj("blacks"); onMoved: (v) => lab.setValue("blacks", v) }

                            SectionHeader { title: "Presence" }
                            AdjustSlider { label: "Clarity"; value: adj("clarity"); onMoved: (v) => lab.setValue("clarity", v) }
                            AdjustSlider { label: "Dehaze"; value: adj("dehaze"); onMoved: (v) => lab.setValue("dehaze", v) }
                            AdjustSlider { label: "Vibrance"; value: adj("vibrance"); onMoved: (v) => lab.setValue("vibrance", v) }
                            AdjustSlider { label: "Saturation"; value: adj("saturation"); onMoved: (v) => lab.setValue("saturation", v) }

                            Button {
                                text: "Reset all"
                                flat: true
                                Layout.alignment: Qt.AlignRight
                                onClicked: lab.resetAdjustments()
                            }
                            Item { implicitHeight: 12 }
                        }
                    }

                    // ---------------------------------------------------- looks
                    ScrollView {
                        id: looksScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: looksScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 6

                            SectionHeader { title: "LUTs on this photo" }
                            Text {
                                visible: lab.luts.length === 0
                                text: "None. Click LUTs in the collection below to switch them on; several stack, and every click shows at once."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Repeater {
                                model: lab.luts
                                delegate: Rectangle {
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    implicitHeight: lutCol.implicitHeight + 16
                                    radius: 6
                                    color: Theme.panelRaised
                                    ColumnLayout {
                                        id: lutCol
                                        anchors.fill: parent
                                        anchors.margins: 8
                                        spacing: 0
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Text {
                                                text: (index + 1) + ". " + modelData.file.replace(/\.[^.]+$/, "")
                                                color: Theme.text
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            ToolButton {
                                                text: "▲"
                                                enabled: index > 0
                                                onClicked: lab.moveLut(index, index - 1)
                                                ToolTip.visible: hovered
                                                ToolTip.text: "apply earlier"
                                            }
                                            ToolButton {
                                                text: "▼"
                                                enabled: index < lab.luts.length - 1
                                                onClicked: lab.moveLut(index, index + 1)
                                                ToolTip.visible: hovered
                                                ToolTip.text: "apply later"
                                            }
                                            ToolButton {
                                                text: "✕"
                                                onClicked: lab.removeLut(index)
                                            }
                                        }
                                        AdjustSlider {
                                            label: "Strength"
                                            from: 0; to: 100; neutral: 100; unit: " %"
                                            value: modelData.strength
                                            onMoved: (v) => lab.setLutStrength(index, v)
                                        }
                                    }
                                }
                            }
                            Button {
                                visible: lab.luts.length > 1
                                text: "Remove all"
                                flat: true
                                Layout.alignment: Qt.AlignRight
                                onClicked: lab.clearLuts()
                            }

                            SectionHeader { title: "LUT collection" }
                            RowLayout {
                                Layout.fillWidth: true
                                TextField {
                                    id: lutFilter
                                    placeholderText: "search LUTs"
                                    Layout.fillWidth: true
                                }
                                ToolButton {
                                    text: "Import"
                                    onClicked: lutDialog.open()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "add .cube, .3dl or .png LUT files; pick as many as you like"
                                }
                                ToolButton {
                                    text: "Folder"
                                    onClicked: lutFolderDialog.open()
                                    ToolTip.visible: hovered
                                    ToolTip.text: "add every LUT in a folder"
                                }
                            }
                            Text {
                                visible: lab.lutCollection.length === 0
                                text: "Your collection is empty. Import LUT files or a whole folder of them."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Repeater {
                                model: lab.lutCollection.filter(f => f.toLowerCase().indexOf(lutFilter.text.toLowerCase()) >= 0)
                                delegate: ItemDelegate {
                                    id: lutItem
                                    required property string modelData
                                    readonly property int position: lab.luts.findIndex(l => l.file === modelData)
                                    Layout.fillWidth: true
                                    padding: 6
                                    highlighted: position >= 0
                                    enabled: lab.hasImage
                                    onClicked: lab.toggleLut(modelData)
                                    contentItem: RowLayout {
                                        spacing: 8
                                        Rectangle {
                                            implicitWidth: 22; implicitHeight: 22; radius: 11
                                            color: lutItem.position >= 0 ? Theme.accent : "transparent"
                                            border.width: lutItem.position >= 0 ? 0 : 2
                                            border.color: Theme.textDim
                                            Text {
                                                anchors.centerIn: parent
                                                text: lutItem.position >= 0 ? lutItem.position + 1 : ""
                                                color: Theme.window
                                                font.bold: true
                                                font.pixelSize: 12
                                            }
                                        }
                                        Text {
                                            text: lutItem.modelData.replace(/\.[^.]+$/, "")
                                            color: Theme.text
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                            }

                            // looks are LUTs now; a photo edited before that can still
                            // carry a darktable style, so it can at least be taken off
                            RowLayout {
                                visible: lab.currentStyle !== ""
                                Layout.fillWidth: true
                                Text {
                                    text: "Older style on this photo: " + lab.currentStyle.replace(/^.*\|/, "")
                                    color: Theme.textDim
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }
                                Button {
                                    text: "Remove"
                                    flat: true
                                    onClicked: lab.applyStyle("")
                                }
                            }
                            Item { implicitHeight: 12 }
                        }
                    }

                    // --------------------------------------------------- detail
                    ScrollView {
                        id: detailScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: detailScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4
                            SectionHeader { title: "Noise reduction" }
                            AdjustSlider {
                                label: "Noise reduction"
                                from: 0; to: 100
                                value: adj("noise")
                                onMoved: (v) => lab.setValue("noise", v)
                            }
                            Text {
                                text: "Uses darktable's profiled denoise for your camera model."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }

                            SectionHeader { title: "AI denoise" }
                            Text {
                                text: !lab.aiBuilt
                                      ? "This build has no AI support."
                                      : "Cleans up noise in raw files with a neural network running on this computer; nothing is uploaded. "
                                        + "The result is a new DNG next to the raw (name_raw-denoise.dng), which opens here to keep editing."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Switch {
                                visible: lab.aiBuilt
                                text: "Enable AI features"
                                checked: lab.aiEnabled
                                onToggled: lab.setAiEnabled(checked)
                                Layout.fillWidth: true
                            }
                            RowLayout {
                                visible: lab.aiBuilt && lab.aiEnabled && lab.aiModel === 0
                                Layout.fillWidth: true
                                Button {
                                    visible: lab.aiDownload < 0
                                    text: "Download the denoise model"
                                    highlighted: true
                                    onClicked: lab.downloadAiModel()
                                }
                                ProgressBar {
                                    visible: lab.aiDownload >= 0
                                    from: 0; to: 1
                                    value: lab.aiDownload
                                    Layout.fillWidth: true
                                }
                                Button {
                                    visible: lab.aiDownload >= 0
                                    text: "Cancel"
                                    flat: true
                                    onClicked: lab.cancelAiDownload()
                                }
                            }
                            ColumnLayout {
                                visible: lab.aiBuilt && lab.aiEnabled && lab.aiModel === 1
                                Layout.fillWidth: true
                                spacing: 4
                                AdjustSlider {
                                    id: aiStrength
                                    label: "Strength"
                                    from: 0; to: 100; neutral: 100; unit: " %"
                                    value: 100
                                    onMoved: (v) => value = v
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Button {
                                        text: lab.aiDenoising ? "Denoising..." : "AI Denoise"
                                        highlighted: true
                                        enabled: lab.hasImage && !lab.aiDenoising
                                        onClicked: lab.aiDenoise(aiStrength.value)
                                    }
                                    BusyIndicator {
                                        running: lab.aiDenoising
                                        visible: running
                                        implicitWidth: 32; implicitHeight: 32
                                    }
                                }
                            }
                            Text {
                                visible: lab.aiBuilt && lab.aiEnabled && lab.aiModel < 0
                                text: "No AI denoise model is known to this build."
                                color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                    }

                    // --------------------------------------------------- optics
                    ScrollView {
                        id: opticsScroll
                        contentWidth: availableWidth
                        ColumnLayout {
                            width: opticsScroll.availableWidth - 2 * Theme.gutter
                            x: Theme.gutter
                            spacing: 4
                            SectionHeader { title: "Lens" }
                            Switch {
                                text: "Lens correction (automatic, from the camera and lens data)"
                                checked: adj("lens")
                                onToggled: lab.setValue("lens", checked)
                                Layout.fillWidth: true
                            }
                            SectionHeader { title: "Geometry" }
                            AdjustSlider {
                                label: "Straighten"
                                from: -10; to: 10; stepSize: 0.1; decimals: 1; unit: "°"
                                value: adj("straighten")
                                onMoved: (v) => lab.setValue("straighten", v)
                            }
                        }
                    }
                }
            }
        }
    }

    }

    Shortcut { sequences: ["Right"]; enabled: lab.mode === "develop"; onActivated: lab.openNeighbour(1) }
    Shortcut { sequences: ["Left"]; enabled: lab.mode === "develop"; onActivated: lab.openNeighbour(-1) }
    Shortcut { sequences: [StandardKey.Copy]; enabled: lab.mode === "develop"; onActivated: lab.copyEdit() }

    FileDialog {
        id: openDialog
        title: "Open a photo"
        nameFilters: ["Photos (*.cr2 *.cr3 *.nef *.arw *.raf *.orf *.rw2 *.dng *.pef *.srw *.jpg *.jpeg *.tif *.tiff *.png *.heic *.avif)", "All files (*)"]
        onAccepted: lab.openFile(selectedFile)
    }
    FileDialog {
        id: exportDialog
        title: "Export"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "jpg"
        nameFilters: ["JPEG (*.jpg)", "TIFF (*.tif)", "PNG (*.png)", "WebP (*.webp)"]
        onAccepted: lab.exportTo(selectedFile, 92)
    }
    FileDialog {
        id: lutDialog
        title: "Import LUTs into your collection"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["LUTs (*.cube *.3dl *.png)", "All files (*)"]
        onAccepted: lab.importLuts(selectedFiles)
    }
    FolderDialog {
        id: lutFolderDialog
        title: "Import every LUT in a folder"
        onAccepted: lab.importLuts([selectedFolder])
    }
}
