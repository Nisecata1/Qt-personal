#include <QDir>
#include <QDateTime>
#include <QMessageBox>
#include <QMutexLocker>
#include <QThread>
#include <QTimer>

#include "controller.h"
#include "devicemsg.h"
#include "decoder.h"
#include "device.h"
#include "filehandler.h"
#include "recorder.h"
#include "server.h"
#include "demuxer.h"

namespace qsc {

namespace {
AVPacket *clonePacket(const AVPacket *packet)
{
    if (!packet) {
        return Q_NULLPTR;
    }

    AVPacket *clone = av_packet_alloc();
    if (!clone) {
        return Q_NULLPTR;
    }

    if (av_packet_ref(clone, packet) < 0) {
        av_packet_free(&clone);
        return Q_NULLPTR;
    }
    return clone;
}

void freePacket(AVPacket *packet)
{
    if (!packet) {
        return;
    }
    av_packet_unref(packet);
    av_packet_free(&packet);
}

QString normalizeRecordFormat(const QString &recordFileFormat)
{
    const QString format = recordFileFormat.trimmed().toLower();
    if (format == QLatin1String("mp4") || format == QLatin1String("mkv")) {
        return format;
    }
    return QString();
}
}

Device::Device(DeviceParams params, QObject *parent) : IDevice(parent), m_params(params)
{
    if (params.display) {
        m_decoder = new Decoder([this](int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV) {
            for (const auto& item : m_deviceObservers) {
                item->onFrame(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
            }
        }, this);
    }

    m_fileHandler = new FileHandler(this);
    m_controller = new Controller([this](const QByteArray& buffer) -> qint64 {
        // 将控制消息写入当前设备的 control socket，socket 不存在时按写入失败处理。
        if (!m_server || !m_server->getControlSocket()) {
            return 0;
        }

        return m_server->getControlSocket()->write(buffer.data(), buffer.length());
    }, params.serial, params.gameScript, this, [this]() -> qint64 {
        // 暴露 control socket 待写字节数，用于远端光标丢弃过期位置。
        if (!m_server || !m_server->getControlSocket()) {
            return 0;
        }

        return m_server->getControlSocket()->bytesToWrite();
    });

    m_stream = new Demuxer(this);
    m_bitRateIdleTimer = new QTimer(this);
    m_bitRateIdleTimer->setInterval(1000);
    connect(m_bitRateIdleTimer, &QTimer::timeout, this, [this]() {
        if (!m_serverStartSuccess || m_lastBitRateValue == 0 || !m_lastBitRateUpdateElapsed.isValid()) {
            return;
        }
        if (m_lastBitRateUpdateElapsed.elapsed() <= 1500) {
            return;
        }
        m_lastBitRateValue = 0;
        for (const auto& item : m_deviceObservers) {
            item->updateBitRate(0);
        }
    });
    m_bitRateIdleTimer->start();

    m_server = new Server(this);
    initSignals();
}

Device::~Device()
{
    Device::disconnectDevice();
}

void Device::setUserData(void *data)
{
    m_userData = data;
}

void *Device::getUserData()
{
    return m_userData;
}

void Device::registerDeviceObserver(DeviceObserver *observer)
{
    m_deviceObservers.insert(observer);
}

void Device::deRegisterDeviceObserver(DeviceObserver *observer)
{
    m_deviceObservers.erase(observer);
}

const QString &Device::getSerial()
{
    return m_params.serial;
}

QString Device::createRecordingFilePath(const QString &recordDir, const QString &recordFileFormat, QString *errorOut) const
{
    auto setError = [errorOut](const QString &message) {
        if (errorOut) {
            *errorOut = message;
        }
    };

    const QString format = normalizeRecordFormat(recordFileFormat);
    if (format.isEmpty()) {
        setError(QStringLiteral("Unsupported record format: %1").arg(recordFileFormat));
        return QString();
    }

    const QString trimmedDir = recordDir.trimmed();
    if (trimmedDir.isEmpty()) {
        setError(QStringLiteral("Please select a record save path first."));
        return QString();
    }

    QDir dir(trimmedDir);
    if (!dir.exists() && !QDir().mkpath(trimmedDir)) {
        setError(QStringLiteral("Could not create record directory: %1").arg(trimmedDir));
        return QString();
    }

    QString safeSerial = m_params.serial.trimmed();
    if (safeSerial.isEmpty()) {
        safeSerial = QStringLiteral("device");
    }
    const QString invalidChars = QStringLiteral("\\/:*?\"<>|");
    for (const QChar &invalidChar : invalidChars) {
        safeSerial.replace(invalidChar, QLatin1Char('_'));
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    const QString fileName = QStringLiteral("%1_%2.%3").arg(safeSerial, timestamp, format);
    return dir.absoluteFilePath(fileName);
}

bool Device::startRecording(const QString &recordDir, const QString &recordFileFormat, QString *errorOut)
{
    auto setError = [errorOut](const QString &message) {
        if (errorOut) {
            *errorOut = message;
        }
    };

    QString filePath = createRecordingFilePath(recordDir, recordFileFormat, errorOut);
    if (filePath.isEmpty()) {
        return false;
    }

    QSize streamFrameSize;
    AVPacket *cachedConfigPacket = Q_NULLPTR;
    bool hadCachedConfigPacket = false;
    {
        QMutexLocker locker(&m_recordingMutex);
        if (m_recordingActive || m_recorder) {
            setError(QStringLiteral("Recording is already in progress."));
            return false;
        }
        if (!m_serverStartSuccess) {
            setError(QStringLiteral("Device session is not connected."));
            return false;
        }
        if (!m_streamFrameSize.isValid()) {
            setError(QStringLiteral("Video stream is not ready yet. Please try again in a moment."));
            return false;
        }

        streamFrameSize = m_streamFrameSize;
        hadCachedConfigPacket = m_lastRecorderConfigPacket != Q_NULLPTR;
        cachedConfigPacket = clonePacket(m_lastRecorderConfigPacket);
    }

    if (hadCachedConfigPacket && !cachedConfigPacket) {
        setError(QStringLiteral("Could not copy the current video config packet for recording."));
        return false;
    }

    Recorder *recorder = new Recorder(filePath, this);
    recorder->setFrameSize(streamFrameSize);
    if (!recorder->open()) {
        setError(recorder->lastError().isEmpty()
            ? QStringLiteral("Could not open recorder.")
            : recorder->lastError());
        delete recorder;
        freePacket(cachedConfigPacket);
        return false;
    }

    if (!recorder->startRecorder()) {
        setError(recorder->lastError().isEmpty()
            ? QStringLiteral("Could not start recorder.")
            : recorder->lastError());
        recorder->close();
        delete recorder;
        freePacket(cachedConfigPacket);
        return false;
    }

    {
        QMutexLocker locker(&m_recordingMutex);
        if (m_recordingActive || m_recorder) {
            setError(QStringLiteral("Recording is already in progress."));
            if (recorder->isRunning()) {
                recorder->stopRecorder();
                recorder->wait();
            }
            recorder->close();
            delete recorder;
            freePacket(cachedConfigPacket);
            return false;
        }

        m_recorder = recorder;
        m_recordingActive = true;
        m_activeRecordingFilePath = filePath;
    }

    emit recordingStateChanged(m_params.serial, true, filePath);

    if (cachedConfigPacket) {
        const bool pushOk = recorder->push(cachedConfigPacket);
        freePacket(cachedConfigPacket);
        if (!pushOk) {
            setError(recorder->lastError().isEmpty()
                ? QStringLiteral("Could not send cached video config packet to recorder.")
                : recorder->lastError());
            stopRecorderInternal(true);
            return false;
        }
    }

    return true;
}

void Device::stopRecording()
{
    stopRecorderInternal(true);
}

bool Device::isRecording() const
{
    QMutexLocker locker(&m_recordingMutex);
    return m_recordingActive && !m_recorder.isNull();
}

void Device::stopRecorderInternal(bool emitSignal)
{
    Recorder *recorder = Q_NULLPTR;
    bool wasActive = false;
    {
        QMutexLocker locker(&m_recordingMutex);
        recorder = m_recorder.data();
        wasActive = m_recordingActive;
        m_recorder = Q_NULLPTR;
        m_recordingActive = false;
        m_activeRecordingFilePath.clear();
    }

    if (!recorder && !(emitSignal && wasActive)) {
        return;
    }

    auto cleanup = [this, recorder, emitSignal, wasActive]() {
        if (recorder) {
            if (recorder->isRunning()) {
                recorder->stopRecorder();
                recorder->wait();
            }
            recorder->close();
            delete recorder;
        }

        if (emitSignal && wasActive) {
            emit recordingStateChanged(m_params.serial, false, QString());
        }
    };

    if (QThread::currentThread() == thread()) {
        cleanup();
    } else {
        QTimer::singleShot(0, this, cleanup);
    }
}

void Device::failRecorder(const QString &message)
{
    QString errorMessage = message.trimmed();
    {
        QMutexLocker locker(&m_recordingMutex);
        if (!m_recorder && !m_recordingActive) {
            return;
        }
        if (errorMessage.isEmpty() && m_recorder) {
            errorMessage = m_recorder->lastError();
        }
    }

    if (errorMessage.isEmpty()) {
        errorMessage = QStringLiteral("Recording stopped because the recorder failed.");
    }

    stopRecorderInternal(true);

    auto emitError = [this, errorMessage]() {
        emit recordingError(m_params.serial, errorMessage);
    };
    if (QThread::currentThread() == thread()) {
        emitError();
    } else {
        QTimer::singleShot(0, this, emitError);
    }
}

void Device::cacheRecorderConfigPacket(const AVPacket *packet)
{
    AVPacket *clonedPacket = clonePacket(packet);
    if (packet && !clonedPacket) {
        qWarning("Could not cache recorder config packet");
        return;
    }

    QMutexLocker locker(&m_recordingMutex);
    freePacket(m_lastRecorderConfigPacket);
    m_lastRecorderConfigPacket = clonedPacket;
}

void Device::clearCachedRecorderConfigPacket()
{
    QMutexLocker locker(&m_recordingMutex);
    freePacket(m_lastRecorderConfigPacket);
    m_lastRecorderConfigPacket = Q_NULLPTR;
}

void Device::updateScript(QString script)
{
    if (m_controller) {
        m_controller->updateScript(script);
    }
}

void Device::screenshot()
{
    if (!m_decoder) {
        return;
    }

    // screenshot
    m_decoder->peekFrame([this](int width, int height, uint8_t* dataRGB32) {
       saveFrame(width, height, dataRGB32);
    });
}

void Device::showTouch(bool show)
{
    AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->setShowTouchesEnabled(getSerial(), show);

    qInfo() << getSerial() << " show touch " << (show ? "enable" : "disable");
}

bool Device::isReversePort(quint16 port)
{
    if (m_server && m_server->isReverse() && port == m_server->getParams().localPort) {
        return true;
    }

    return false;
}

void Device::initSignals()
{
    if (m_controller) {
        connect(m_controller, &Controller::grabCursor, this, [this](bool grab){
            for (const auto& item : m_deviceObservers) {
                item->grabCursor(grab);
            }
        });
    }
    if (m_fileHandler) {
        connect(m_fileHandler, &FileHandler::fileHandlerResult, this, [this](FileHandler::FILE_HANDLER_RESULT processResult, bool isApk) {
            QString tipsType = "";
            if (isApk) {
                tipsType = "install apk";
            } else {
                tipsType = "file transfer";
            }
            QString tips;
            if (FileHandler::FAR_IS_RUNNING == processResult) {
                tips = QString("wait current %1 to complete").arg(tipsType);
            }
            if (FileHandler::FAR_SUCCESS_EXEC == processResult) {
                tips = QString("%1 complete, save in %2").arg(tipsType).arg(m_params.pushFilePath);
            }
            if (FileHandler::FAR_ERROR_EXEC == processResult) {
                tips = QString("%1 failed").arg(tipsType);
            }
            qInfo() << tips;
        });
    }

    if (m_server) {
        connect(m_server, &Server::serverStarted, this, [this](bool success, const QString &deviceName, const QSize &size, int initialOrientation) {
            m_serverStartSuccess = success;
            const QSize streamSize = size;
            {
                QMutexLocker locker(&m_recordingMutex);
                m_streamFrameSize = success ? streamSize : QSize();
            }
            QSize uiSize = streamSize;
            if (success && m_server && m_server->isControlMapToScreenEnabled()) {
                const QSize controlSize = m_server->getControlReferenceSize();
                if (controlSize.isValid()) {
                    uiSize = controlSize;
                }
            }
            emit deviceConnected(success, m_params.serial, deviceName, uiSize, initialOrientation);
            if (success) {
                double diff = m_startTimeCount.elapsed() / 1000.0;
                qInfo() << QString("server start finish in %1s").arg(diff).toStdString().c_str();
                const bool videoEnabled = m_server->isVideoEnabled();

                if (m_params.recordFile) {
                    QString errorString;
                    if (!startRecording(m_params.recordPath, m_params.recordFileFormat, &errorString) && !errorString.isEmpty()) {
                        emit recordingError(m_params.serial, errorString);
                    }
                }

                // init decoder
                if (videoEnabled && m_decoder) {
                    m_decoder->open();
                }

                if (videoEnabled) {
                    // init stream
                    auto socket = m_server->removeVideoSocket();
                    if (!socket) {
                        qCritical("Video enabled but no video socket found");
                        m_server->stop();
                        return;
                    }
                    m_stream->installVideoSocket(socket);
                    m_stream->setFrameSize(streamSize);
                    m_stream->startDecode();
                }

                // recv device msg
                connect(m_server->getControlSocket(), &QTcpSocket::readyRead, this, [this]() {
                    if (!m_controller) {
                        return;
                    }

                    auto controlSocket = m_server->getControlSocket();
                    while (controlSocket->bytesAvailable()) {
                        QByteArray byteArray = controlSocket->peek(controlSocket->bytesAvailable());
                        DeviceMsg deviceMsg;
                        qint32 consume = deviceMsg.deserialize(byteArray);
                        if (0 >= consume) {
                            break;
                        }
                        controlSocket->read(consume);
                        m_controller->recvDeviceMsg(&deviceMsg);
                    }
                });

                // 鏄剧ず鐣岄潰鏃舵墠鑷姩鎭睆锛坢_params.display锛?
                if (m_params.closeScreen && m_params.display && m_controller) {
                    m_controller->setDisplayPower(false);
                }
            } else {
                m_server->stop();
            }
        });
        connect(m_server, &Server::serverStoped, this, [this]() {
            disconnectDevice();
            qDebug() << "server process stop";
        });
    }

    if (m_stream) {
        connect(m_stream, &Demuxer::onStreamStop, this, [this]() {
            disconnectDevice();
            qDebug() << "stream thread stop";
        });
        connect(m_stream, &Demuxer::updateBitRate, this, [this](quint64 bitRate) {
            m_lastBitRateValue = bitRate;
            m_lastBitRateUpdateElapsed.restart();
            for (const auto& item : m_deviceObservers) {
                item->updateBitRate(bitRate);
            }
        });
        connect(m_stream, &Demuxer::getFrame, this, [this](AVPacket *packet) {
            if (m_decoder && !m_decoder->push(packet)) {
                qCritical("Could not send packet to decoder");
            }

            Recorder *recorder = Q_NULLPTR;
            {
                QMutexLocker locker(&m_recordingMutex);
                recorder = m_recorder.data();
            }
            if (recorder && !recorder->push(packet)) {
                failRecorder(recorder->lastError().isEmpty()
                    ? QStringLiteral("Could not send packet to recorder.")
                    : recorder->lastError());
            }
        }, Qt::DirectConnection);
        connect(m_stream, &Demuxer::getConfigFrame, this, [this](AVPacket *packet) {
            cacheRecorderConfigPacket(packet);

            Recorder *recorder = Q_NULLPTR;
            {
                QMutexLocker locker(&m_recordingMutex);
                recorder = m_recorder.data();
            }
            if (recorder && !recorder->push(packet)) {
                failRecorder(recorder->lastError().isEmpty()
                    ? QStringLiteral("Could not send config packet to recorder.")
                    : recorder->lastError());
            }
        }, Qt::DirectConnection);
    }

    if (m_decoder) {
        connect(m_decoder, &Decoder::updateFPS, this, [this](quint32 fps) {
            for (const auto& item : m_deviceObservers) {
                item->updateFPS(fps);
            }
        });
    }
}

bool Device::connectDevice()
{
    if (!m_server || m_serverStartSuccess) {
        return false;
    }

    // fix: macos cant recv finished signel, timer is ok
    QTimer::singleShot(0, this, [this]() {
        m_startTimeCount.start();
        // max size support 480p 720p 1080p 璁惧鍘熺敓鍒嗚鲸鐜?
        // support wireless connect, example:
        //m_server->start("192.168.0.174:5555", 27183, m_maxSize, m_bitRate, "");
        // only one devices, serial can be null
        // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
        Server::ServerParams params;
        params.serverLocalPath = m_params.serverLocalPath;
        params.serverRemotePath = m_params.serverRemotePath;
        params.serial = m_params.serial;
        params.localPort = m_params.localPort;
        params.maxSize = m_params.maxSize;
        params.bitRate = m_params.bitRate;
        params.maxFps = m_params.maxFps;
        params.useReverse = m_params.useReverse;
        params.captureOrientationLock = m_params.captureOrientationLock;
        params.captureOrientation = m_params.captureOrientation;
        params.stayAwake = m_params.stayAwake;
        params.serverVersion = m_params.serverVersion;
        params.logLevel = m_params.logLevel;
        params.codecOptions = m_params.codecOptions;
        params.codecName = m_params.codecName;
        params.scid = m_params.scid;

        params.crop = "";
        params.control = true;
        m_server->start(params);
    });

    return true;
}

void Device::disconnectDevice()
{
    // 停设备服务前先释放当前会话注入到手机上的触点，避免下一次连上后鼠标点击像“失灵”。
    if (!m_server) {
        return;
    }

    if (m_controller) {
        m_controller->resetInputState();
    }

    for (const auto& item : m_deviceObservers) {
        item->grabCursor(false);
        item->updateBitRate(0);
    }
    m_lastBitRateValue = 0;
    m_lastBitRateUpdateElapsed.invalidate();

    stopRecorderInternal(true);
    clearCachedRecorderConfigPacket();
    {
        QMutexLocker locker(&m_recordingMutex);
        m_streamFrameSize = QSize();
    }

    m_server->stop();
    m_server = Q_NULLPTR;

    if (m_stream) {
        m_stream->stopDecode();
    }

    // server must stop before decoder, because decoder block main thread
    if (m_decoder) {
        m_decoder->close();
    }

    if (m_serverStartSuccess) {
        emit deviceDisconnected(m_params.serial);
    }
    m_serverStartSuccess = false;
}

void Device::postGoBack()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoBack();

    for (const auto& item : m_deviceObservers) {
        item->postGoBack();
    }
}

void Device::postGoHome()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoHome();

    for (const auto& item : m_deviceObservers) {
        item->postGoHome();
    }
}

void Device::postGoMenu()
{
    if (!m_controller) {
        return;
    }
    m_controller->postGoMenu();

    for (const auto& item : m_deviceObservers) {
        item->postGoMenu();
    }
}

void Device::postAppSwitch()
{
    if (!m_controller) {
        return;
    }
    m_controller->postAppSwitch();

    for (const auto& item : m_deviceObservers) {
        item->postAppSwitch();
    }
}

void Device::postPower()
{
    if (!m_controller) {
        return;
    }
    m_controller->postPower();

    for (const auto& item : m_deviceObservers) {
        item->postPower();
    }
}

void Device::postVolumeUp()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeUp();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeUp();
    }
}

void Device::postVolumeDown()
{
    if (!m_controller) {
        return;
    }
    m_controller->postVolumeDown();

    for (const auto& item : m_deviceObservers) {
        item->postVolumeDown();
    }
}

void Device::postCopy()
{
    if (!m_controller) {
        return;
    }
    m_controller->copy();

    for (const auto& item : m_deviceObservers) {
        item->postCopy();
    }
}

void Device::postCut()
{
    if (!m_controller) {
        return;
    }
    m_controller->cut();

    for (const auto& item : m_deviceObservers) {
        item->postCut();
    }
}

void Device::setDisplayPower(bool on)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDisplayPower(on);

    for (const auto& item : m_deviceObservers) {
        item->setDisplayPower(on);
    }
}

void Device::expandNotificationPanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->expandNotificationPanel();

    for (const auto& item : m_deviceObservers) {
        item->expandNotificationPanel();
    }
}

void Device::collapsePanel()
{
    if (!m_controller) {
        return;
    }
    m_controller->collapsePanel();

    for (const auto& item : m_deviceObservers) {
        item->collapsePanel();
    }
}

void Device::postBackOrScreenOn(bool down)
{
    if (!m_controller) {
        return;
    }
    m_controller->postBackOrScreenOn(down);

    for (const auto& item : m_deviceObservers) {
        item->postBackOrScreenOn(down);
    }
}

void Device::postTextInput(QString &text)
{
    if (!m_controller) {
        return;
    }
    m_controller->postTextInput(text);

    for (const auto& item : m_deviceObservers) {
        item->postTextInput(text);
    }
}

void Device::setDeviceClipboardText(QString &text, bool paste)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDeviceClipboardText(text, paste);

    for (const auto& item : m_deviceObservers) {
        item->setDeviceClipboardText(text, paste);
    }
}

void Device::requestDeviceClipboard()
{
    if (!m_controller) {
        return;
    }
    m_controller->requestDeviceClipboard();

    for (const auto& item : m_deviceObservers) {
        item->requestDeviceClipboard();
    }
}

void Device::setDeviceClipboard(bool pause)
{
    if (!m_controller) {
        return;
    }
    m_controller->setDeviceClipboard(pause);

    for (const auto& item : m_deviceObservers) {
        item->setDeviceClipboard(pause);
    }
}

void Device::clipboardPaste()
{
    if (!m_controller) {
        return;
    }
    m_controller->clipboardPaste();

    for (const auto& item : m_deviceObservers) {
        item->clipboardPaste();
    }
}

void Device::pushFileRequest(const QString &file, const QString &devicePath)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onPushFileRequest(getSerial(), file, devicePath);

    for (const auto& item : m_deviceObservers) {
        item->pushFileRequest(file, devicePath);
    }
}

void Device::installApkRequest(const QString &apkFile)
{
    if (!m_fileHandler) {
        return;
    }
    m_fileHandler->onInstallApkRequest(getSerial(), apkFile);

    for (const auto& item : m_deviceObservers) {
        item->installApkRequest(apkFile);
    }
}

void Device::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->mouseEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->mouseEvent(from, frameSize, showSize);
    }
}

void Device::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->wheelEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->wheelEvent(from, frameSize, showSize);
    }
}

void Device::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!m_controller) {
        return;
    }
    m_controller->keyEvent(from, frameSize, showSize);

    for (const auto& item : m_deviceObservers) {
        item->keyEvent(from, frameSize, showSize);
    }
}

bool Device::isCurrentCustomKeymap()
{
    if (!m_controller) {
        return false;
    }
    return m_controller->isCurrentCustomKeymap();
}

bool Device::saveFrame(int width, int height, uint8_t* dataRGB32)
{
    if (!dataRGB32) {
        return false;
    }

    QImage rgbImage(dataRGB32, width, height, QImage::Format_RGB32);

    // save
    QString absFilePath;
    QString fileDir(m_params.recordPath);
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        return false;
    }
    QDateTime dateTime = QDateTime::currentDateTime();
    QString fileName = dateTime.toString("_yyyyMMdd_hhmmss_zzz");
    fileName = m_params.serial + fileName;
    fileName.replace(":", "_");
    fileName.replace(".", "_");
    fileName += ".png";
    QDir dir(fileDir);
    absFilePath = dir.absoluteFilePath(fileName);
    int ret = rgbImage.save(absFilePath, "PNG", 100);
    if (!ret) {
        return false;
    }

    qInfo() << "screenshot save to " << absFilePath;
    return true;
}

}


