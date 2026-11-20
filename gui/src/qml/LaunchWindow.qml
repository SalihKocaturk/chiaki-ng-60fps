// Pre-launch frame generation settings (see framegenlaunch.cpp).
// Plain QtQuick only, so the app-wide Material style does not apply here.
import QtQuick
import QtQuick.Window

Window {
    id: win
    width: 400
    height: 560
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    x: Screen.virtualX + (Screen.width - width) / 2
    y: Screen.virtualY + (Screen.height - height) / 2
    visible: true
    title: "chiaki-ng"
    color: "#0d0520"

    readonly property color ink: "#fff6fb"
    readonly property color mute: "#d9c3e6"
    readonly property color hot: "#ff2e88"
    readonly property color orange: "#ff8a3d"
    readonly property color gold: "#ffc857"
    readonly property color cyan: "#5ef2ff"
    readonly property var bufferValues: [0, 4, 8, 12, 16, 25, 33]
    readonly property bool canEdit: launch.supported && launch.frameGenEnabled

    FontLoader { id: anton; source: "qrc:/launch/Anton-Regular.ttf" }

    // ---- strings (this window only) ----
    readonly property var strings: ({
        en:    { frames: "In-between frames", quality: "Quality", high: "High", balanced: "Balanced", fast: "Fast",
                 buffer: "Buffer", bufferHint: "%1 ms · smoother ↔ lower latency", supported: "Supported",
                 unsupported: "This graphics card is not supported", cont: "CONTINUE", enter: "Press Enter to continue" },
        tr:    { frames: "Ara kare üretimi", quality: "Kalite", high: "Yüksek", balanced: "Dengeli", fast: "Hızlı",
                 buffer: "Tampon", bufferHint: "%1 ms · daha akıcı ↔ daha az gecikme", supported: "Destekleniyor",
                 unsupported: "Bu ekran kartı desteklenmiyor", cont: "DEVAM", enter: "Devam etmek için Enter'a bas" },
        de:    { frames: "Zwischenbilder", quality: "Qualität", high: "Hoch", balanced: "Ausgewogen", fast: "Schnell",
                 buffer: "Puffer", bufferHint: "%1 ms · flüssiger ↔ weniger Latenz", supported: "Unterstützt",
                 unsupported: "Diese Grafikkarte wird nicht unterstützt", cont: "WEITER", enter: "Enter zum Fortfahren" },
        es:    { frames: "Fotogramas intermedios", quality: "Calidad", high: "Alta", balanced: "Equilibrada", fast: "Rápida",
                 buffer: "Búfer", bufferHint: "%1 ms · más fluido ↔ menos latencia", supported: "Compatible",
                 unsupported: "Esta tarjeta gráfica no es compatible", cont: "CONTINUAR", enter: "Pulsa Intro para continuar" },
        fr:    { frames: "Images intermédiaires", quality: "Qualité", high: "Élevée", balanced: "Équilibrée", fast: "Rapide",
                 buffer: "Tampon", bufferHint: "%1 ms · plus fluide ↔ moins de latence", supported: "Compatible",
                 unsupported: "Cette carte graphique n'est pas prise en charge", cont: "CONTINUER", enter: "Appuyez sur Entrée pour continuer" },
        pt_BR: { frames: "Quadros intermediários", quality: "Qualidade", high: "Alta", balanced: "Equilibrada", fast: "Rápida",
                 buffer: "Buffer", bufferHint: "%1 ms · mais fluido ↔ menos latência", supported: "Compatível",
                 unsupported: "Esta placa de vídeo não é compatível", cont: "CONTINUAR", enter: "Pressione Enter para continuar" },
        it:    { frames: "Fotogrammi intermedi", quality: "Qualità", high: "Alta", balanced: "Bilanciata", fast: "Veloce",
                 buffer: "Buffer", bufferHint: "%1 ms · più fluido ↔ meno latenza", supported: "Supportata",
                 unsupported: "Questa scheda grafica non è supportata", cont: "CONTINUA", enter: "Premi Invio per continuare" },
        ru:    { frames: "Промежуточные кадры", quality: "Качество", high: "Высокое", balanced: "Баланс", fast: "Быстрое",
                 buffer: "Буфер", bufferHint: "%1 мс · плавнее ↔ меньше задержка", supported: "Поддерживается",
                 unsupported: "Эта видеокарта не поддерживается", cont: "ПРОДОЛЖИТЬ", enter: "Нажмите Enter, чтобы продолжить" },
        ja:    { frames: "中間フレーム生成", quality: "画質", high: "高", balanced: "バランス", fast: "高速",
                 buffer: "バッファ", bufferHint: "%1 ms · なめらか ↔ 低遅延", supported: "対応",
                 unsupported: "このグラフィックカードは非対応です", cont: "続行", enter: "Enter キーで続行" },
        zh_CN: { frames: "中间帧生成", quality: "质量", high: "高", balanced: "平衡", fast: "快速",
                 buffer: "缓冲", bufferHint: "%1 毫秒 · 更流畅 ↔ 更低延迟", supported: "支持",
                 unsupported: "此显卡不受支持", cont: "继续", enter: "按 Enter 继续" }
    })
    readonly property var lang: {
        const name = launch.language;
        if (strings[name]) return strings[name];
        const prefix = name.split(/[_-]/)[0];
        if (prefix === "pt") return strings.pt_BR;
        if (prefix === "zh") return strings.zh_CN;
        return strings[prefix] || strings.en;
    }
    function t(key) { return lang[key] || strings.en[key]; }

    // ---- scene ----
    Image {
        anchors.fill: parent
        source: "qrc:/launch/scene.svg"
        sourceSize: Qt.size(width * Screen.devicePixelRatio, height * Screen.devicePixelRatio)
        smooth: true
    }

    Item {
        id: content
        anchors.fill: parent
        anchors.margins: 20
        focus: true
        Keys.onReturnPressed: launch.accept()
        Keys.onEnterPressed: launch.accept()

        opacity: 0
        Component.onCompleted: { opacity = 1; forceActiveFocus(); }
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        // Title + GPU chip
        Column {
            anchors.left: parent.left
            anchors.top: parent.top
            spacing: -4
            Repeater {
                model: [["FRAME", win.ink], ["GENERATION", "#ffe27a"]]
                Text {
                    text: modelData[0]
                    color: modelData[1]
                    font.family: anton.name
                    font.pixelSize: 36
                    style: Text.Raised
                    styleColor: "#5a3a0f4f"
                }
            }
        }

        Rectangle {
            id: chip
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 6
            height: 26
            width: Math.min(chipRow.implicitWidth + 20, 190)
            radius: 13
            color: "#8c0e061c"
            border.color: launch.supported ? "#595ef2ff" : "#80ff8a3d"
            Row {
                id: chipRow
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    width: 7; height: 7; radius: 3.5
                    anchors.verticalCenter: parent.verticalCenter
                    color: launch.supported ? win.cyan : win.orange
                }
                Text {
                    text: launch.gpuName.length ? launch.gpuName.replace("NVIDIA GeForce ", "") : "GPU"
                    color: launch.supported ? win.cyan : win.orange
                    font.pixelSize: 11.5
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, 150)
                }
            }
        }

        // Settings panel
        Rectangle {
            id: panel
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: unsupportedNote.visible ? unsupportedNote.top : button.top
            anchors.bottomMargin: 14
            height: rows.implicitHeight + 8
            radius: 14
            color: "#a6160828"
            border.color: "#24ffffff"

            Column {
                id: rows
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter

                // On/off
                Item {
                    width: parent.width; height: 52
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        Text { text: "30 → 60 fps"; color: win.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Text { text: win.t("frames"); color: win.mute; font.pixelSize: 11.5 }
                    }
                    Rectangle {
                        id: toggle
                        readonly property bool on: launch.supported && launch.frameGenEnabled
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 46; height: 26; radius: 13
                        opacity: launch.supported ? 1 : 0.4
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: toggle.on ? win.hot : "#33ffffff" }
                            GradientStop { position: 1; color: toggle.on ? win.orange : "#33ffffff" }
                        }
                        Rectangle {
                            width: 20; height: 20; radius: 10
                            y: 3
                            x: toggle.on ? toggle.width - width - 3 : 3
                            color: "white"
                            Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -8
                            enabled: launch.supported
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            onClicked: launch.frameGenEnabled = !launch.frameGenEnabled
                        }
                    }
                }

                Rectangle { width: parent.width; height: 1; color: "#14ffffff" }

                // Quality
                Item {
                    width: parent.width; height: 48
                    opacity: win.canEdit ? 1 : 0.45
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: win.t("quality"); color: win.ink; font.pixelSize: 14; font.weight: Font.DemiBold
                    }
                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: seg.implicitWidth + 4; height: 28; radius: 9
                        color: "#14ffffff"
                        Row {
                            id: seg
                            anchors.centerIn: parent
                            Repeater {
                                model: ["high", "balanced", "fast"]
                                Rectangle {
                                    readonly property bool sel: launch.quality === index
                                    width: segText.implicitWidth + 20; height: 24; radius: 7
                                    color: sel ? "white" : "transparent"
                                    Behavior on color { ColorAnimation { duration: 140 } }
                                    Text {
                                        id: segText
                                        anchors.centerIn: parent
                                        text: win.t(modelData)
                                        color: parent.sel ? "#3a0f4f" : win.mute
                                        font.pixelSize: 12
                                        font.weight: parent.sel ? Font.Bold : Font.Normal
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: win.canEdit
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: launch.quality = index
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle { width: parent.width; height: 1; color: "#14ffffff" }

                // Buffer
                Item {
                    width: parent.width; height: 54
                    opacity: win.canEdit ? 1 : 0.45
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        Text { text: win.t("buffer"); color: win.ink; font.pixelSize: 14; font.weight: Font.DemiBold }
                        Text { text: win.t("bufferHint").arg(launch.bufferMs); color: win.mute; font.pixelSize: 11.5 }
                    }
                    Item {
                        id: slider
                        readonly property int idx: {
                            let best = 0;
                            for (let i = 1; i < win.bufferValues.length; i++)
                                if (Math.abs(win.bufferValues[i] - launch.bufferMs) < Math.abs(win.bufferValues[best] - launch.bufferMs))
                                    best = i;
                            return best;
                        }
                        readonly property real frac: idx / (win.bufferValues.length - 1)
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 130; height: 24
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width; height: 6; radius: 3
                            color: "#24ffffff"
                        }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: slider.frac * parent.width; height: 6; radius: 3
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0; color: "#35d0c0" }
                                GradientStop { position: 1; color: win.cyan }
                            }
                        }
                        Rectangle {
                            width: 16; height: 16; radius: 8
                            anchors.verticalCenter: parent.verticalCenter
                            x: slider.frac * parent.width - width / 2
                            color: "white"
                            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                        }
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -8
                            enabled: win.canEdit
                            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                            function pick(px) {
                                const f = Math.max(0, Math.min(1, (px - 8) / slider.width));
                                launch.bufferMs = win.bufferValues[Math.round(f * (win.bufferValues.length - 1))];
                            }
                            onPressed: (m) => pick(m.x)
                            onPositionChanged: (m) => { if (pressed) pick(m.x); }
                        }
                    }
                }
            }
        }

        Text {
            id: unsupportedNote
            visible: !launch.supported
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: button.top
            anchors.bottomMargin: 12
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: win.t("unsupported")
            color: win.gold
            font.pixelSize: 12.5
            font.weight: Font.DemiBold
        }

        // Continue
        Rectangle {
            id: button
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: hint.top
            anchors.bottomMargin: 8
            height: 50
            radius: 12
            scale: buttonArea.pressed ? 0.97 : 1
            Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: win.hot }
                GradientStop { position: 0.7; color: win.orange }
                GradientStop { position: 1; color: win.gold }
            }
            Text {
                anchors.centerIn: parent
                text: win.t("cont")
                color: "white"
                font.family: anton.name
                font.pixelSize: 20
                font.letterSpacing: 1.2
            }
            MouseArea {
                id: buttonArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: launch.accept()
            }
        }

        Text {
            id: hint
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            text: win.t("enter")
            color: win.mute
            font.pixelSize: 11
        }
    }
}
