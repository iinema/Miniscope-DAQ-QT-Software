import QtQuick 2.0
import QtQuick.Controls 2.12
import QtQuick.Layouts 1.12

Item {
    id: root
    objectName: "root"
    width: parent.width
    height: parent.height

    property double currentRecordTime: 0
    property double ucRecordLength: 1

    signal submitNoteSignal(string note)

    Rectangle {
        id: rectangle
        color: "#cacaca"
        anchors.fill: parent
    }

    GridLayout {
        id: gridLayout
        anchors.fill: parent
        rows: 6
        columns: 2

        TextField {
            id: textNote
            objectName: "textNote"
            text: qsTr("Enter Note")
            font.pointSize: 10
            font.family: "Arial"
            Layout.fillWidth: true
            onFocusChanged:{
                      if(focus)
                          selectAll()
                 }
        }
        Button {
            id: bNoteSubmit
            objectName: "bNoteSubmit"
            text: qsTr("Submit Note")
            enabled: false
            checkable: false
            Layout.fillWidth: true
            onClicked: {

                root.submitNoteSignal(textNote.text);
                textNote.text = "Enter Note";
            }

        }
        Switch {
            id: element
            text: qsTr("Triggerable")
        }

        ScrollView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
//            Layout.row:5
//            Layout.column: 0
            Layout.columnSpan: 2

            TextArea {
                id: messageTextArea
                objectName: "messageTextArea"
                text: "Messages:\n"
                wrapMode: Text.WrapAnywhere
                anchors.fill: parent
                font.pointSize: 12
                readOnly: true
                background: Rectangle {
//                    radius: rbSelectUserConfig.radius
                    anchors.fill: parent
//                    border.width: 1
                    color: "#ebebeb"
                }
            }
        }

        DelayButton {
            id: bRecord
            objectName: "bRecord"
            text: qsTr("Record")
//            autoExclusive: true
            font.family: "Arial"
            font.pointSize: 12
            Layout.fillWidth: true
            delay:1000
//            Layout.row: 0
//            Layout.column: 0
            Layout.rowSpan: 1
            Layout.columnSpan: 1
            onActivated: {

                bNoteSubmit.enabled = true;
                bStop.enabled = true;
                bRecord.enabled = false;
            }
        }

        DelayButton {
            id: bStop
            objectName: "bStop"
            text: qsTr("Stop")
            enabled: false
//            autoExclusive: true
            font.pointSize: 12
            font.family: "Arial"
//            enabled: false
            checkable: true
            Layout.fillWidth: true
            delay:1000
//            Layout.row: 0
//            Layout.column: 1
            Layout.rowSpan: 1
            Layout.columnSpan: 1
            onActivated: {

                bNoteSubmit.enabled = false;
                bStop.enabled = false;
                bRecord.enabled = true;
            }

        }

        ProgressBar {
            id: progressBar
            height: 40
            Layout.minimumHeight: 40
//            Layout.preferredHeight: 40
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            Layout.fillWidth: true
            value: root.currentRecordTime/root.ucRecordLength
            from: 0
            to:1
            Layout.columnSpan: 1
        }

        Text {
            id: recordTimeText
            objectName: "recordTimeText"

            text: root.currentRecordTime.toString() + "/" + root.ucRecordLength.toString() + "s"
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            font.pointSize: 12
            font.family: "Arial"
        }
    }
}




/*##^##
Designer {
    D{i:1;anchors_height:200;anchors_width:200}D{i:2;anchors_height:200;anchors_width:200}
}
##^##*/