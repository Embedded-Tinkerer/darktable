import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

// Lightroom-style library: folders, a thumbnail grid, multi-select, and
// actions that apply to every selected photo
Item {
    id: root
    property bool wide: true
    readonly property int selectedCount: lab.selection.length
    readonly property var labelColors: ["#e5484d", "#f5c400", "#30a46c", "#3e63dd", "#8e4ec6"]

    function isSelected(id) { return lab.selection.indexOf(id) >= 0 }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ folders
        Rectangle {
            visible: root.wide
            Layout.fillHeight: true
            Layout.preferredWidth: 250
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 6

                Button {
                    text: "+ Import folder"
                    highlighted: true
                    Layout.fillWidth: true
                    onClicked: folderDialog.open()
                }
                SectionHeader { title: "Folders" }
                ListView {
                    id: folderList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: [{ id: -1, name: "All photographs", count: -1 }].concat(lab.folders)
                    delegate: ItemDelegate {
                        required property var modelData
                        width: folderList.width
                        highlighted: lab.currentFolder === modelData.id
                        onClicked: lab.showFolder(modelData.id)
                        contentItem: RowLayout {
                            Text {
                                text: modelData.name
                                color: Theme.text
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: modelData.count >= 0 ? modelData.count : ""
                                color: Theme.textDim
                            }
                        }
                        ToolTip.visible: hovered && modelData.path !== undefined
                        ToolTip.text: modelData.path || ""
                    }
                }
            }
        }

        // --------------------------------------------------------------- grid
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // filter bar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 48
                color: Theme.window
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8
                    ToolButton {
                        visible: !root.wide
                        text: "Import"
                        onClicked: folderDialog.open()
                    }
                    Text { text: "Show"; color: Theme.textDim }
                    ComboBox {
                        id: ratingFilter
                        implicitWidth: 150
                        model: ["all", "★ and up", "★★ and up", "★★★ and up",
                                "★★★★ and up", "★★★★★", "rejected"]
                        currentIndex: lab.showRejected ? 6 : lab.minRating
                        onActivated: (i) => {
                            if(i === 6) lab.setShowRejected(true)
                            else { lab.setShowRejected(false); lab.setMinRating(i) }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: root.selectedCount > 0 ? root.selectedCount + " of " + lab.images.length + " selected"
                                                     : lab.images.length + " photos"
                        color: Theme.textDim
                    }
                    ToolButton { text: "Select all"; onClicked: lab.selectAll() }
                    ToolButton {
                        text: "None"
                        enabled: root.selectedCount > 0
                        onClicked: lab.clearSelection()
                    }
                    Slider {
                        id: zoom
                        visible: root.wide
                        from: 120; to: 360; value: 200
                        implicitWidth: 120
                    }
                }
            }

            GridView {
                id: grid
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                focus: true
                readonly property int columns: Math.max(2, Math.floor(width / (root.wide ? zoom.value : 130)))
                cellWidth: Math.floor(width / columns)
                cellHeight: cellWidth
                model: lab.images
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: cell
                    required property var modelData
                    required property int index
                    width: grid.cellWidth
                    height: grid.cellHeight
                    readonly property bool selected: root.isSelected(modelData.id)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 4
                        color: cell.selected ? Theme.panelRaised : Theme.panel
                        border.width: cell.selected ? 2 : 0
                        border.color: Theme.accent

                        Image {
                            anchors.fill: parent
                            anchors.margins: 8
                            anchors.bottomMargin: 26
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: false
                            sourceSize.width: 400
                            sourceSize.height: 400
                            source: "image://thumb/" + cell.modelData.id + "/"
                                    + (lab.thumbVersions[String(cell.modelData.id)] || 0)
                            opacity: cell.modelData.rating === 6 ? 0.35 : 1
                        }

                        RowLayout {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: 6
                            spacing: 4
                            Text {
                                text: cell.modelData.rating === 6 ? "✕"
                                      : "★".repeat(cell.modelData.rating)
                                color: cell.modelData.rating === 6 ? "#e5484d" : "#f5c400"
                                font.pixelSize: 12
                            }
                            Repeater {
                                model: 5
                                delegate: Rectangle {
                                    required property int index
                                    visible: (cell.modelData.labels >> index) & 1
                                    width: 9; height: 9; radius: 5
                                    color: root.labelColors[index]
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                visible: cell.modelData.edited
                                text: "●"
                                color: Theme.accent
                                font.pixelSize: 10
                                ToolTip.visible: false
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: (m) => {
                            grid.forceActiveFocus()
                            lab.select(cell.modelData.id,
                                       (m.modifiers & Qt.ShiftModifier) ? 2
                                       : (m.modifiers & Qt.ControlModifier) ? 1 : 0)
                        }
                        onDoubleClicked: lab.openImage(cell.modelData.id)
                        onPressAndHold: lab.select(cell.modelData.id, 1) // touch: add to selection
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: lab.images.length === 0
                    text: lab.ready ? "No photos here yet. Import a folder to start." : "Starting the darktable engine..."
                    color: Theme.textDim
                    font.pixelSize: 16
                }
            }

            // ---------------------------------------------------- export progress
            Rectangle {
                visible: lab.exporting
                Layout.fillWidth: true
                implicitHeight: exportRow.implicitHeight + 12
                color: Theme.panelRaised
                RowLayout {
                    id: exportRow
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    spacing: 12
                    Text {
                        text: "Exporting " + Math.min(lab.exportDone + 1, lab.exportTotal) + " of " + lab.exportTotal
                              + (lab.exportFile ? "  " + lab.exportFile : "")
                        color: Theme.text
                        elide: Text.ElideMiddle
                        Layout.preferredWidth: 320
                    }
                    ProgressBar {
                        from: 0
                        to: Math.max(1, lab.exportTotal)
                        value: lab.exportDone
                        Layout.fillWidth: true
                    }
                    Button {
                        text: "Cancel"
                        flat: true
                        onClicked: lab.cancelExport()
                    }
                }
            }

            // ----------------------------------------------- selection actions
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: actions.implicitHeight + 16
                color: Theme.panel
                enabled: root.selectedCount > 0

                Flow {
                    id: actions
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.margins: 8
                    spacing: 4

                    Repeater {
                        model: 6
                        delegate: ToolButton {
                            required property int index
                            text: index === 0 ? "0★" : index + "★"
                            onClicked: lab.rateSelected(index)
                            ToolTip.visible: hovered
                            ToolTip.text: "rate " + index + " (key " + index + ")"
                        }
                    }
                    ToolButton {
                        text: "Reject"
                        onClicked: lab.rejectSelected()
                        ToolTip.visible: hovered
                        ToolTip.text: "key X"
                    }
                    Repeater {
                        model: 5
                        delegate: ToolButton {
                            required property int index
                            contentItem: Rectangle {
                                implicitWidth: 14; implicitHeight: 14; radius: 7
                                color: root.labelColors[index]
                            }
                            onClicked: lab.toggleLabelSelected(index)
                        }
                    }
                    ToolSeparator {}
                    Button {
                        text: "+ Add LUTs to selected"
                        highlighted: true
                        onClicked: { lutPopup.picked = []; lutPopup.open() }
                    }
                    Button {
                        text: "Style..."
                        onClicked: stylePopup.open()
                    }
                    Button {
                        text: "Paste edit"
                        enabled: lab.hasCopiedEdit
                        onClicked: lab.pasteEditToSelected()
                        ToolTip.visible: hovered
                        ToolTip.text: lab.hasCopiedEdit ? "paste the copied sliders, LUTs and style (Ctrl+V)"
                                                        : "copy an edit in Develop first"
                    }
                    Button {
                        text: "Export..."
                        enabled: !lab.exporting
                        onClicked: exportPopup.open()
                    }
                }
            }
        }
    }

    Popup {
        id: stylePopup
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        height: Math.min(root.height - 32, 560)
        modal: true
        ColumnLayout {
            anchors.fill: parent
            Text {
                text: "Apply a style to " + root.selectedCount + " photos"
                color: Theme.text
                font.pixelSize: 16
            }
            TextField {
                id: styleSearch
                placeholderText: "search styles"
                Layout.fillWidth: true
            }
            ListView {
                id: styleList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: [""].concat(lab.styles.filter(s => s.toLowerCase().indexOf(styleSearch.text.toLowerCase()) >= 0))
                delegate: ItemDelegate {
                    required property string modelData
                    width: styleList.width
                    text: modelData === "" ? "none (remove style)" : modelData.replace(/^.*\|/, "")
                    onClicked: { lab.styleSelected(modelData); stylePopup.close() }
                }
            }
        }
    }

    // several LUTs from the collection, in the order they are picked
    Popup {
        id: lutPopup
        property var picked: []
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        height: Math.min(root.height - 32, 600)
        modal: true
        ColumnLayout {
            anchors.fill: parent
            Text {
                text: "Add LUTs to " + root.selectedCount + " photos"
                color: Theme.text
                font.pixelSize: 16
            }
            Text {
                text: "Pick one or more; they stack on top of each photo's own LUTs in the order picked."
                color: Theme.textDim
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: lutSearch
                    placeholderText: "search LUTs"
                    Layout.fillWidth: true
                }
                ToolButton { text: "Import"; onClicked: lutDialog.open() }
            }
            ListView {
                id: lutList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: lab.lutCollection.filter(f => f.toLowerCase().indexOf(lutSearch.text.toLowerCase()) >= 0)
                delegate: ItemDelegate {
                    id: pick
                    required property string modelData
                    readonly property int position: lutPopup.picked.indexOf(modelData)
                    width: lutList.width
                    highlighted: position >= 0
                    onClicked: {
                        const p = lutPopup.picked.slice()
                        if(position >= 0) p.splice(position, 1)
                        else p.push(modelData)
                        lutPopup.picked = p
                    }
                    contentItem: RowLayout {
                        spacing: 8
                        Rectangle {
                            implicitWidth: 22; implicitHeight: 22; radius: 11
                            color: pick.position >= 0 ? Theme.accent : "transparent"
                            border.width: pick.position >= 0 ? 0 : 2
                            border.color: Theme.textDim
                            Text {
                                anchors.centerIn: parent
                                text: pick.position >= 0 ? pick.position + 1 : ""
                                color: Theme.window
                                font.bold: true
                                font.pixelSize: 12
                            }
                        }
                        Text {
                            text: pick.modelData.replace(/\.[^.]+$/, "")
                            color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; flat: true; onClicked: lutPopup.close() }
                Button {
                    text: lutPopup.picked.length > 1 ? "Add " + lutPopup.picked.length + " LUTs" : "Add LUT"
                    highlighted: true
                    enabled: lutPopup.picked.length > 0
                    onClicked: { lab.addLutsToSelected(lutPopup.picked); lutPopup.close() }
                }
            }
        }
    }

    Shortcut { sequences: ["0"]; enabled: root.visible; onActivated: lab.rateSelected(0) }
    Shortcut { sequences: ["1"]; enabled: root.visible; onActivated: lab.rateSelected(1) }
    Shortcut { sequences: ["2"]; enabled: root.visible; onActivated: lab.rateSelected(2) }
    Shortcut { sequences: ["3"]; enabled: root.visible; onActivated: lab.rateSelected(3) }
    Shortcut { sequences: ["4"]; enabled: root.visible; onActivated: lab.rateSelected(4) }
    Shortcut { sequences: ["5"]; enabled: root.visible; onActivated: lab.rateSelected(5) }
    Shortcut { sequences: ["X"]; enabled: root.visible; onActivated: lab.rejectSelected() }
    Shortcut { sequences: [StandardKey.SelectAll]; enabled: root.visible; onActivated: lab.selectAll() }
    Shortcut { sequences: [StandardKey.Paste]; enabled: root.visible; onActivated: lab.pasteEditToSelected() }

    FolderDialog {
        id: folderDialog
        title: "Import a folder (subfolders included)"
        onAccepted: lab.importFolder(selectedFolder, true)
    }
    FolderDialog {
        id: exportFolderDialog
        title: "Export the selected photos to"
        onAccepted: exportPopup.folder = selectedFolder
    }

    // batch export: where, as what, how big, and how the files are named
    Popup {
        id: exportPopup
        property url folder: lab.lastExportFolder
        property bool openWhenDone: false
        anchors.centerIn: parent
        width: Math.min(root.width - 32, 520)
        modal: true

        function folderText(u) { return u.toString().replace(/^file:\/\//, "") }
        function options() {
            return {
                format: formatBox.currentValue,
                quality: Math.round(qualitySlider.value),
                maxSize: sizeBox.currentValue >= 0 ? sizeBox.currentValue : customSize.value,
                naming: namingBox.currentValue,
                suffix: suffixField.text,
                base: baseField.text
            }
        }
        function example() {
            const ext = "." + formatBox.currentValue
            if(namingBox.currentValue === "sequence") return (baseField.text || "photo") + "_001" + ext
            return "DSC01234" + (namingBox.currentValue === "suffix" ? suffixField.text : "") + ext
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Text {
                text: "Export " + root.selectedCount + " photo" + (root.selectedCount === 1 ? "" : "s")
                color: Theme.text
                font.pixelSize: 16
            }

            SectionHeader { title: "Folder" }
            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: exportPopup.folder.toString() ? exportPopup.folderText(exportPopup.folder) : "choose a folder"
                    color: exportPopup.folder.toString() ? Theme.text : Theme.textDim
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
                Button { text: "Choose..."; onClicked: exportFolderDialog.open() }
            }

            SectionHeader { title: "File" }
            GridLayout {
                columns: 2
                columnSpacing: 12
                Layout.fillWidth: true
                Text { text: "Format"; color: Theme.text }
                ComboBox {
                    id: formatBox
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { value: "jpg", text: "JPEG" },
                        { value: "tif", text: "TIFF (16-bit)" },
                        { value: "png", text: "PNG" },
                        { value: "webp", text: "WebP" }
                    ]
                }
                Text {
                    text: "Quality " + Math.round(qualitySlider.value)
                    color: Theme.text
                    visible: qualitySlider.visible
                }
                Slider {
                    id: qualitySlider
                    visible: formatBox.currentValue === "jpg" || formatBox.currentValue === "webp"
                    from: 50; to: 100; stepSize: 1; value: 92
                    Layout.fillWidth: true
                }
                Text { text: "Size"; color: Theme.text }
                RowLayout {
                    Layout.fillWidth: true
                    ComboBox {
                        id: sizeBox
                        Layout.fillWidth: true
                        textRole: "text"
                        valueRole: "value"
                        model: [
                            { value: 0, text: "Full size" },
                            { value: 4096, text: "4096 px long edge" },
                            { value: 2560, text: "2560 px long edge" },
                            { value: 2048, text: "2048 px (web, print proofs)" },
                            { value: 1080, text: "1080 px (Instagram)" },
                            { value: -1, text: "Custom" }
                        ]
                    }
                    SpinBox {
                        id: customSize
                        visible: sizeBox.currentValue < 0
                        from: 256; to: 16384; stepSize: 100; value: 3000
                        editable: true
                    }
                }
            }

            SectionHeader { title: "Names" }
            GridLayout {
                columns: 2
                columnSpacing: 12
                Layout.fillWidth: true
                Text { text: "Name"; color: Theme.text }
                ComboBox {
                    id: namingBox
                    Layout.fillWidth: true
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { value: "original", text: "Original name" },
                        { value: "suffix", text: "Original name + text" },
                        { value: "sequence", text: "Name + number" }
                    ]
                }
                Text { text: "Text"; color: Theme.text; visible: suffixField.visible }
                TextField {
                    id: suffixField
                    visible: namingBox.currentValue === "suffix"
                    text: "_edit"
                    Layout.fillWidth: true
                }
                Text { text: "Name"; color: Theme.text; visible: baseField.visible }
                TextField {
                    id: baseField
                    visible: namingBox.currentValue === "sequence"
                    text: "photo"
                    Layout.fillWidth: true
                }
            }
            Text {
                text: "e.g. " + exportPopup.example() + ". Existing files are never overwritten."
                color: Theme.textDim
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            CheckBox {
                text: "Open the folder when done"
                checked: exportPopup.openWhenDone
                onToggled: exportPopup.openWhenDone = checked
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button { text: "Cancel"; flat: true; onClicked: exportPopup.close() }
                Button {
                    text: "Export " + root.selectedCount
                    highlighted: true
                    enabled: exportPopup.folder.toString() !== "" && root.selectedCount > 0
                    onClicked: {
                        exportPopup.pendingOpen = exportPopup.openWhenDone
                        lab.exportSelected(exportPopup.folder, exportPopup.options())
                        exportPopup.close()
                    }
                }
            }
        }

        property bool pendingOpen: false
        Connections {
            target: lab
            function onExportChanged() {
                if(!lab.exporting && exportPopup.pendingOpen && lab.lastExportFolder.toString()) {
                    exportPopup.pendingOpen = false
                    Qt.openUrlExternally(lab.lastExportFolder)
                }
            }
        }
    }
    FileDialog {
        id: lutDialog
        title: "Import LUTs into your collection"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["LUTs (*.cube *.3dl *.png)", "All files (*)"]
        onAccepted: lab.importLuts(selectedFiles)
    }
}
