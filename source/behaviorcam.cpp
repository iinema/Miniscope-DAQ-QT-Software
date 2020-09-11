#include "behaviorcam.h"
#include "newquickview.h"
#include "videodisplay.h"

#include <QQuickView>
#include <QQuickItem>
#include <QSemaphore>
#include <QObject>
#include <QTimer>
#include <QAtomicInt>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QQmlApplicationEngine>
#include <QVector>
#include <QVariant>

BehaviorCam::BehaviorCam(QObject *parent, QJsonObject ucBehavCam) :
    QObject(parent),
    m_camConnected(false),
    behavCamStream(nullptr),
    rootObject(nullptr),
    vidDisplay(nullptr),
    m_previousDisplayFrameNum(0),
    m_acqFrameNum(new QAtomicInt(0)),
    m_daqFrameNum(new QAtomicInt(0)),
    m_streamHeadOrientationState(false),
    m_camCalibWindowOpen(false),
    m_camCalibRunning(false),
    m_roiIsDefined(false)
{

    m_ucBehavCam = ucBehavCam; // hold user config for this Miniscope

    getBehavCamConfig(m_ucBehavCam["deviceType"].toString()); // holds specific Miniscope configuration

    parseUserConfigBehavCam();

    // TODO: Handle cases where there is more than webcams and MiniCAMs
    if (m_ucBehavCam["deviceType"].toString() == "WebCam") {
        isMiniCAM = false;
        m_daqFrameNum = nullptr;
    }
    else {
        isMiniCAM = true;
    }

    // Checks to make sure user config and miniscope device type are supporting BNO streaming
    m_streamHeadOrientationState = false;


    // Thread safe buffer stuff
    freeFrames = new QSemaphore;
    usedFrames = new QSemaphore;
    freeFrames->release(FRAME_BUFFER_SIZE);
    // -------------------------

    // Setup OpenCV camera stream
    behavCamStream = new VideoStreamOCV(nullptr,  m_cBehavCam["width"].toInt(-1), m_cBehavCam["height"].toInt(-1), m_cBehavCam["pixelClock"].toDouble(-1));
    behavCamStream->setDeviceName(m_deviceName);

    behavCamStream->setHeadOrientationConfig(false, false); // don't allow head orientation streaming for behavior cameras
    behavCamStream->setIsColor(m_cBehavCam["isColor"].toBool(false));

    m_camConnected = behavCamStream->connect2Camera(m_ucBehavCam["deviceID"].toInt());
    if (m_camConnected == 0) {
        qDebug() << "Not able to connect and open " << m_ucBehavCam["deviceName"].toString();
    }
    else {
        behavCamStream->setBufferParameters(frameBuffer,
                                             timeStampBuffer,
                                             nullptr,
                                             FRAME_BUFFER_SIZE,
                                             freeFrames,
                                             usedFrames,
                                             m_acqFrameNum,
                                             m_daqFrameNum);


        // -----------------

        // Threading and connections for thread stuff
        videoStreamThread = new QThread;
        behavCamStream->moveToThread(videoStreamThread);

    //    QObject::connect(miniscopeStream, SIGNAL (error(QString)), this, SLOT (errorString(QString)));
        QObject::connect(videoStreamThread, SIGNAL (started()), behavCamStream, SLOT (startStream()));
    //    QObject::connect(miniscopeStream, SIGNAL (finished()), videoStreamThread, SLOT (quit()));
    //    QObject::connect(miniscopeStream, SIGNAL (finished()), miniscopeStream, SLOT (deleteLater()));
        QObject::connect(videoStreamThread, SIGNAL (finished()), videoStreamThread, SLOT (deleteLater()));

        // Pass send message signal through
        QObject::connect(behavCamStream, &VideoStreamOCV::sendMessage, this, &BehaviorCam::sendMessage);

        // Handle request for reinitialization of commands
        QObject::connect(behavCamStream, &VideoStreamOCV::requestInitCommands, this, &BehaviorCam::handleInitCommandsRequest);

        // Pass new Frame available through to parent
//        QObject::connect(behavCamStream, &VideoStreamOCV::newFrameAvailable, this, &BehaviorCam::newFrameAvailable);
        // ----------------------------------------------

        connectSnS();

        if (isMiniCAM)
            sendInitCommands();

        videoStreamThread->start();

        QThread::msleep(500);
    }
}

void BehaviorCam::createView()
{
    if (m_camConnected != 0) {
        if (m_camConnected == 1)
             sendMessage(m_deviceName + " connected using Direct Show.");
        else if (m_camConnected == 2)
            sendMessage(m_deviceName + " couldn't connect using Direct Show. Using computer's default backend.");
        qmlRegisterType<VideoDisplay>("VideoDisplay", 1, 0, "VideoDisplay");

        // Setup Miniscope window

        // TODO: Check deviceType and log correct qml file
    //    const QUrl url("qrc:/" + m_deviceType + ".qml");
        const QUrl url(m_cBehavCam["qmlFile"].toString("qrc:/behaviorCam.qml"));
//        const QUrl url("qrc:/behaviorCam.qml");
        view = new NewQuickView(url);
        view->setWidth(m_cBehavCam["width"].toInt() * m_ucBehavCam["windowScale"].toDouble(1));
        view->setHeight(m_cBehavCam["height"].toInt() * m_ucBehavCam["windowScale"].toDouble(1));

        view->setTitle(m_deviceName);
        view->setX(m_ucBehavCam["windowX"].toInt(1));
        view->setY(m_ucBehavCam["windowY"].toInt(1));

#ifdef Q_OS_WINDOWS
        view->setFlags(Qt::Window | Qt::MSWindowsFixedSizeDialogHint | Qt::WindowTitleHint);
#endif
        view->show();
        // --------------------

        rootObject = view->rootObject();

        QObject::connect(rootObject, SIGNAL( saturationSwitchChanged(bool) ),
                             this, SLOT( handleSaturationSwitchChanged(bool) ));

        configureBehavCamControls();
        vidDisplay = rootObject->findChild<VideoDisplay*>("vD");
        vidDisplay->setMaxBuffer(FRAME_BUFFER_SIZE);
        vidDisplay->setWindowScaleValue(m_ucBehavCam["windowScale"].toDouble(1));

        // Turn on or off saturation display
        if (m_ucBehavCam["showSaturation"].toBool(false)) {
            vidDisplay->setShowSaturation(1);
            rootObject->findChild<QQuickItem*>("saturationSwitch")->setProperty("checked", true);
        }
        else {
            vidDisplay->setShowSaturation(0);
            rootObject->findChild<QQuickItem*>("saturationSwitch")->setProperty("checked", false);
        }

        QObject::connect(rootObject, SIGNAL( takeScreenShotSignal() ),
                             this, SLOT( handleTakeScreenShotSignal() ));
        QObject::connect(rootObject, SIGNAL( vidPropChangedSignal(QString, double, double, double) ),
                             this, SLOT( handlePropChangedSignal(QString, double, double, double) ));

        // Open OpenCV properties dialog for behav cam
        if (!isMiniCAM) {
            rootObject->findChild<QQuickItem*>("camProps")->setProperty("visible", true);
            QObject::connect(rootObject, SIGNAL( camPropsClicked() ), this, SLOT( handleCamPropsClicked()));
            QObject::connect(this, SIGNAL( openCamPropsDialog()), behavCamStream, SLOT( openCamPropsDialog()));
        }

        // Set ROI Stuff
        QObject::connect(rootObject, SIGNAL( setRoiClicked() ), this, SLOT( handleSetRoiClicked()));


        // Handle camera calibration signals from GUI
        QObject::connect(rootObject, SIGNAL( calibrateCameraClicked() ), this, SLOT( handleCamCalibClicked()));
        QObject::connect(rootObject, SIGNAL( calibrateCameraStart() ), this, SLOT( handleCamCalibStart()));
        QObject::connect(rootObject, SIGNAL( calibrateCameraQuit() ), this, SLOT( handleCamCalibQuit()));

        //
        QObject::connect(view, &NewQuickView::closing, behavCamStream, &VideoStreamOCV::stopSteam);
        QObject::connect(vidDisplay->window(), &QQuickWindow::beforeRendering, this, &BehaviorCam::sendNewFrame);

        // Link up ROI signal and slot
        QObject::connect(vidDisplay, &VideoDisplay::newROISignal, this, &BehaviorCam::handleNewROI);

        sendMessage(m_deviceName + " is connected.");
        if (m_ucBehavCam.contains("ROI")) {
            vidDisplay->setROI({(int)round(m_roiBoundingBox[0] * m_ucBehavCam["windowScale"].toDouble(1)),
                                (int)round(m_roiBoundingBox[1] * m_ucBehavCam["windowScale"].toDouble(1)),
                                (int)round(m_roiBoundingBox[2] * m_ucBehavCam["windowScale"].toDouble(1)),
                                (int)round(m_roiBoundingBox[3] * m_ucBehavCam["windowScale"].toDouble(1)),
                                0});
        }
    }
    else {
        sendMessage("Error: " + m_deviceName + " cannot connect to camera. Check deviceID.");
    }

}

void BehaviorCam::connectSnS(){
    if (isMiniCAM)
        QObject::connect(this, SIGNAL( setPropertyI2C(long, QVector<quint8>) ), behavCamStream, SLOT( setPropertyI2C(long, QVector<quint8>) ));

}

void BehaviorCam::parseUserConfigBehavCam() {
    // Currently not needed. If arrays get added into JSON config then this might
    m_deviceName = m_ucBehavCam["deviceName"].toString("Behavior Cam " + QString::number(m_ucBehavCam["deviceID"].toInt()));
    m_compressionType = m_ucBehavCam["compression"].toString("None");

    if (m_ucBehavCam.contains("ROI")) {
        // User Config defines ROI Bounding Box
        m_roiIsDefined = true;
        m_roiBoundingBox[0] = m_ucBehavCam["ROI"].toObject()["leftEdge"].toInt(-1);
        m_roiBoundingBox[1] = m_ucBehavCam["ROI"].toObject()["topEdge"].toInt(-1);
        m_roiBoundingBox[2] = m_ucBehavCam["ROI"].toObject()["width"].toInt(-1);
        m_roiBoundingBox[3] = m_ucBehavCam["ROI"].toObject()["height"].toInt(-1);
        // TODO: Throw error is values are incorrect or missing
    }
    else {
        m_roiBoundingBox[0] = 0;
        m_roiBoundingBox[1] = 0;
        m_roiBoundingBox[2] = m_cBehavCam["width"].toInt(-1);
        m_roiBoundingBox[3] = m_cBehavCam["height"].toInt(-1);
    }
}

void BehaviorCam::sendInitCommands()
{
    // Sends out the commands in the miniscope json config file under Initialize
    QVector<quint8> packet;
    long preambleKey;
    int tempValue;

    QVector<QMap<QString,int>> sendCommands = parseSendCommand(m_cBehavCam["initialize"].toArray());
    QMap<QString,int> command;

    for (int i = 0; i < sendCommands.length(); i++) {
        // Loop through send commands
        command = sendCommands[i];
        packet.clear();
        if (command["protocol"] == PROTOCOL_I2C) {
            preambleKey = 0;

            packet.append(command["addressW"]);
            preambleKey = (preambleKey<<8) | packet.last();

            for (int j = 0; j < command["regLength"]; j++) {
                packet.append(command["reg" + QString::number(j)]);
                preambleKey = (preambleKey<<8) | packet.last();
            }
            for (int j = 0; j < command["dataLength"]; j++) {
                tempValue = command["data" + QString::number(j)];
                packet.append(tempValue);
                preambleKey = (preambleKey<<8) | packet.last();
            }
//        qDebug() << packet;
//        preambleKey = 0;
//        for (int k = 0; k < (command["regLength"]+1); k++)
//            preambleKey |= (packet[k]&0xFF)<<(8*k);
        emit setPropertyI2C(preambleKey, packet);
        }
        else {
            qDebug() << command["protocol"] << " initialize protocol not yet supported";
        }

    }
}

QString BehaviorCam::getCompressionType()
{
    return m_compressionType;
}

void BehaviorCam::getBehavCamConfig(QString deviceType) {
    QString jsonFile;
    QFile file;
    m_deviceType = deviceType;
//    file.setFileName(":/deviceConfigs/behaviorCams.json");
    file.setFileName("./deviceConfigs/behaviorCams.json");
    file.open(QIODevice::ReadOnly | QIODevice::Text);
    jsonFile = file.readAll();
    file.close();
    QJsonDocument d = QJsonDocument::fromJson(jsonFile.toUtf8());
    QJsonObject jObj = d.object();
    m_cBehavCam = jObj[deviceType].toObject();

}

void BehaviorCam::configureBehavCamControls() {

    QQuickItem *controlItem; // Pointer to VideoPropertyControl in qml for each objectName
    QJsonObject values; // min, max, startingValue, and stepSize for each control used in 'j' loop
    QStringList keys;

    QJsonObject controlSettings = m_cBehavCam["controlSettings"].toObject(); // Get controlSettings from json

    if (controlSettings.isEmpty()) {
        qDebug() << "controlSettings missing from behaviorCams.json for deviceType = " << m_deviceType;
        return;
    }
    QStringList controlName =  controlSettings.keys();
    for (int i = 0; i < controlName.length(); i++) { // Loop through controls
        controlItem = rootObject->findChild<QQuickItem*>(controlName[i]);
//        qDebug() << controlItem;
        values = controlSettings[controlName[i]].toObject();

        if (m_ucBehavCam.contains(controlName[i])) {// sets starting value if it is defined in user config
            if (m_ucBehavCam[controlName[i]].isDouble())
                values["startValue"] = m_ucBehavCam[controlName[i]].toDouble();
            if (m_ucBehavCam[controlName[i]].isString())
                values["startValue"] = m_ucBehavCam[controlName[i]].toString();
        }

        keys = values.keys();
        if (controlItem) {
            controlItem->setVisible(true);
            for (int j = 0; j < keys.size(); j++) { // Set min, max, startValue, and stepSize in order found in 'format'
                if (keys[j] == "sendCommand") {
                    m_controlSendCommand[controlName[i]] = parseSendCommand(values["sendCommand"].toArray());
                }
                else {
                    if (values[keys[j]].isArray()) {
                        QJsonArray tempArray = values[keys[j]].toArray();
                        QVariantList tempVect;
                        for (int k = 0; k < tempArray.size(); k++) {
                            if (tempArray[k].isDouble())
                                tempVect.append(tempArray[k].toDouble());
                            if (tempArray[k].isString())
                                tempVect.append(tempArray[k].toString());
                        }
                        controlItem->setProperty(keys[j].toLatin1().data(), tempVect);
                    }
                    else if (values[keys[j]].isString()) {
                        controlItem->setProperty(keys[j].toLatin1().data(), values[keys[j]].toString());
                        if (keys[j] == "startValue")
                            // sends signal on initial setup of controls
                            emit onPropertyChanged(m_deviceName, controlName[i], values["startValue"].toVariant());
                    }
                    else {
                        controlItem->setProperty(keys[j].toLatin1().data(), values[keys[j]].toDouble());
                        if (keys[j] == "startValue")
                            // sends signal on initial setup of controls
                            emit onPropertyChanged(m_deviceName, controlName[i], values["startValue"].toVariant());
                    }
                }
            }

        }
        else
            qDebug() << controlName[i] << " not found in qml file.";

    }
}

QVector<QMap<QString, int>> BehaviorCam::parseSendCommand(QJsonArray sendCommand)
{
    // Creates a QMap for handing future I2C/SPI slider value send commands
    QVector<QMap<QString, int>> output;
    QMap<QString, int> commandStructure;
    QJsonObject jObj;
    QStringList keys;

    for (int i = 0; i < sendCommand.size(); i++) {
        jObj = sendCommand[i].toObject();
        keys = jObj.keys();

        for (int j = 0; j < keys.size(); j++) {
                // -1 = controlValue, -2 = error
            if (jObj[keys[j]].isString())
                commandStructure[keys[j]] = processString2Int(jObj[keys[j]].toString());
            else if (jObj[keys[j]].isDouble())
                commandStructure[keys[j]] = jObj[keys[j]].toInt();
        }
        output.append(commandStructure);
    }
    return output;
}

int BehaviorCam::processString2Int(QString s)
{
    // Should return a uint8 type of value (0 to 255)
    bool ok = false;
    int value;
    int size = s.size();
    if (size == 0) {
        qDebug() << "No data in string to convert to int";
        value = SEND_COMMAND_ERROR;
        ok = false;
    }
    else if (s.left(2) == "0x"){
        // HEX
        value = s.right(size-2).toUInt(&ok, 16);
    }
    else if (s.left(2) == "0b"){
        // Binary
        value = s.right(size-2).toUInt(&ok, 2);
    }
    else {
        value = s.toUInt(&ok, 10);
//        qDebug() << "String is " << s;
        if (ok == false) {
            // This is then a string
            if (s == "I2C")
                value = PROTOCOL_I2C;
            else if (s == "SPI")
                value = PROTOCOL_SPI;
            else if (s == "valueH24")
                value = SEND_COMMAND_VALUE_H24;
            else if (s == "valueH16")
                value = SEND_COMMAND_VALUE_H16;
            else if (s == "valueH")
                value = SEND_COMMAND_VALUE_H;
            else if (s == "valueL")
                value = SEND_COMMAND_VALUE_L;
            else if (s == "value")
                value = SEND_COMMAND_VALUE;
            else if (s == "value2H")
                value = SEND_COMMAND_VALUE2_H;
            else if (s == "value2L")
                value = SEND_COMMAND_VALUE2_L;
            else
                value = SEND_COMMAND_ERROR;
            ok = true;
        }
    }

    if (ok == true)
        return value;
    else
        return SEND_COMMAND_ERROR;
}

void BehaviorCam::testSlot(QString type, double value)
{
    qDebug() << "IN SLOT!!!!! " << type << " is " << value;
}
void BehaviorCam::sendNewFrame(){
//    vidDisplay->setProperty("displayFrame", QImage("C:/Users/DBAharoni/Pictures/Miniscope/Logo/1.png"));
    int f = *m_acqFrameNum;

    if (f > m_previousDisplayFrameNum) {
        m_previousDisplayFrameNum = f;
        QImage tempFrame2;
//        qDebug() << "Send frame = " << f;
        f = (f - 1)%FRAME_BUFFER_SIZE;

        // TODO: Think about where color to gray and vise versa should take place.
        if (frameBuffer[f].channels() == 1) {
            cv::cvtColor(frameBuffer[f], tempFrame, cv::COLOR_GRAY2BGR);
            tempFrame2 = QImage(tempFrame.data, tempFrame.cols, tempFrame.rows, tempFrame.step, QImage::Format_RGB888);
        }
        else
            tempFrame2 = QImage(frameBuffer[f].data, frameBuffer[f].cols, frameBuffer[f].rows, frameBuffer[f].step, QImage::Format_RGB888);

        vidDisplay->setDisplayFrame(tempFrame2);

        vidDisplay->setBufferUsed(usedFrames->available());
        if (f > 0) // This is just a quick cheat so I don't have to wrap around for (f-1)
            vidDisplay->setAcqFPS(timeStampBuffer[f] - timeStampBuffer[f-1]); // TODO: consider changing name as this is now interframeinterval

        if (isMiniCAM)
            vidDisplay->setDroppedFrameCount(*m_daqFrameNum - *m_acqFrameNum);
        else
            vidDisplay->setDroppedFrameCount(-1);
    }
}

void BehaviorCam::handlePropChangedSignal(QString type, double displayValue, double i2cValue, double i2cValue2)
{
    // type is the objectName of the control
    // value is the control value that was just updated
    QVector<quint8> packet;
    QMap<QString, int> sendCommand;
    int tempValue;
    long preambleKey; // Holds a value that represents the address and reg

    sendMessage(m_deviceName + " " + type + " changed to " + QString::number(displayValue) + ".");
    // Handle props that only affect the user display here
    if (type == "alpha"){
        vidDisplay->setAlpha(displayValue);
    }
    else if (type == "beta") {
        vidDisplay->setBeta(displayValue);
    }
    else {
        // Here handles prop changes that need to be sent over to the Miniscope

        // TODO: maybe add a check to make sure property successfully updates before signallng it has changed
    //    qDebug() << "Sending updated prop signal to backend";

        emit onPropertyChanged(m_deviceName, type, QVariant(displayValue)); // This sends the change to the datasaver


        // TODO: Handle int values greater than 8 bits
        for (int i = 0; i < m_controlSendCommand[type].length(); i++) {
            sendCommand = m_controlSendCommand[type][i];
            packet.clear();
            if (sendCommand["protocol"] == PROTOCOL_I2C) {
                preambleKey = 0;

                packet.append(sendCommand["addressW"]);
                preambleKey = (preambleKey<<8) | packet.last();

                for (int j = 0; j < sendCommand["regLength"]; j++) {
                    packet.append(sendCommand["reg" + QString::number(j)]);
                    preambleKey = (preambleKey<<8) | packet.last();
                }
                for (int j = 0; j < sendCommand["dataLength"]; j++) {
                    tempValue = sendCommand["data" + QString::number(j)];
                    // TODO: Handle value1 through value3
                    if (tempValue == SEND_COMMAND_VALUE_H24) {
                        packet.append((static_cast<quint32>(round(i2cValue))>>24)&0xFF);
                    }
                    else if (tempValue == SEND_COMMAND_VALUE_H16) {
                        packet.append((static_cast<quint32>(round(i2cValue))>>16)&0xFF);
                    }
                    else if (tempValue == SEND_COMMAND_VALUE_H) {
                        packet.append((static_cast<quint32>(round(i2cValue))>>8)&0xFF);
                    }
                    else if (tempValue == SEND_COMMAND_VALUE_L) {
                        packet.append(static_cast<quint32>(round(i2cValue))&0xFF);
                    }
                    else if (tempValue == SEND_COMMAND_VALUE2_H) {
                        packet.append((static_cast<quint32>(round(i2cValue2))>>8)&0xFF);
                    }
                    else if (tempValue == SEND_COMMAND_VALUE2_L) {
                        packet.append(static_cast<quint32>(round(i2cValue2))&0xFF);
                    }
                    else {
                        packet.append(tempValue);
                        preambleKey = (preambleKey<<8) | packet.last();
                    }
                }
    //        qDebug() << packet;

//                for (int k = 0; k < (sendCommand["regLength"]+1); k++)
//                    preambleKey |= (packet[k]&0xFF)<<(8*k);
                emit setPropertyI2C(preambleKey, packet);
            }
            else {
                qDebug() << sendCommand["protocol"] << " protocol for " << type << " not yet supported";
            }
        }
    }
}

void BehaviorCam::handleTakeScreenShotSignal()
{
    // Is called when signal from qml GUI is triggered
    takeScreenShot(m_deviceName);
}

void BehaviorCam::handleCamCalibClicked()
{
    // This slot gets called when user clicks "camera calibration" in behavior cam GUI
    qDebug() << "Entering camera calibration";
    m_camCalibWindowOpen = true;
    // camCalibWindow will open up. This is located in the behaviorCam.qml file
    // This window will display directions begin/quit buttons, and progress of calibration

}

void BehaviorCam::handleCamCalibStart()
{
    qDebug() << "Beginning camera calibration";
    m_camCalibRunning = true;
    // Probably can use an if statement in sendNewFrame() to send frames somewhere for camera calibration
    // Probably want to update the cam calib window that opens up with info as the cam is being calibrated

    // When done, calibration should be saved in a file and the file path should be updated in the user config or
    // Another option would be to just save all the calibration data directly into the user config file

}

void BehaviorCam::handleCamCalibQuit()
{
    qDebug() << "Quitting camera calibration";
    if (m_camCalibRunning) {
        // Do stuff to exit cam calibration algorithm without issue
        m_camCalibRunning = false;
    }
    m_camCalibWindowOpen = false;
}
void BehaviorCam::close()
{
    if (m_camConnected)
        view->close();
}

void BehaviorCam::handleSetRoiClicked()
{
    // TODO: Don't allow this if recording is active!!!!

    // We probably should reset video display to full resolution here before user input of ROI????

    // Tell videodisplay that we will need mouse actions and will need to draw ROI rectangle
    vidDisplay->setROISelectionState(true);


    // TODO: disable ROI Button

}

void BehaviorCam::handleNewROI(int leftEdge, int topEdge, int width, int height)
{
    m_roiIsDefined = true;
    // First scale the local position values to pixel values
    m_roiBoundingBox[0] = round(leftEdge/m_ucBehavCam["windowScale"].toDouble(1));
    m_roiBoundingBox[1] = round(topEdge/m_ucBehavCam["windowScale"].toDouble(1));
    m_roiBoundingBox[2] = round(width/m_ucBehavCam["windowScale"].toDouble(1));
    m_roiBoundingBox[3] = round(height/m_ucBehavCam["windowScale"].toDouble(1));

    if ((m_roiBoundingBox[0] + m_roiBoundingBox[2]) > m_cBehavCam["width"].toInt(-1)) {
        // Edge is off screen
        m_roiBoundingBox[2] = m_cBehavCam["width"].toInt(-1) - m_roiBoundingBox[0];
    }
    if ((m_roiBoundingBox[1] + m_roiBoundingBox[3]) > m_cBehavCam["height"].toInt(-1)) {
        // Edge is off screen
        m_roiBoundingBox[3] = m_cBehavCam["height"].toInt(-1) - m_roiBoundingBox[1];
    }

    sendMessage("ROI Set to [" + QString::number(m_roiBoundingBox[0]) + ", " +
            QString::number(m_roiBoundingBox[1]) + ", " +
            QString::number(m_roiBoundingBox[2]) + ", " +
            QString::number(m_roiBoundingBox[3]) + "]");

    // TODO: Correct ROI if out of bounds

}

void BehaviorCam::handleInitCommandsRequest()
{
    qDebug() << "Reinitializing device.";
    sendInitCommands();
}

void BehaviorCam::handleSaturationSwitchChanged(bool checked)
{
    vidDisplay->setShowSaturation(checked);
}
