#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>

#include <QSettings>

#include "server.h"

#define DEVICE_NAME_FIELD_LENGTH 64
#define SOCKET_NAME_PREFIX "scrcpy"
#define MAX_CONNECT_COUNT 30
#define MAX_RESTART_COUNT 1

static quint32 bufferRead32be(quint8 *buf)
{
    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

static QString resolveUserDataIniPath()
{
    const QString appUserDataPath = QCoreApplication::applicationDirPath() + "/config/userdata.ini";
    QFileInfo appUserDataInfo(appUserDataPath);
    if (appUserDataInfo.exists() && appUserDataInfo.isFile()) {
        return appUserDataPath;
    }

    const QString envConfigPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_CONFIG_PATH"));
    QFileInfo envConfigInfo(envConfigPath);
    if (!envConfigPath.isEmpty() && envConfigInfo.exists() && envConfigInfo.isDir()) {
        return envConfigPath + "/userdata.ini";
    }

    return appUserDataPath;
}

namespace {
bool normalizeRotationValue(const QString &captured, int &orientationOut)
{
    bool ok = false;
    const int value = captured.toInt(&ok);
    if (!ok) {
        return false;
    }

    if (value >= 0 && value <= 3) {
        orientationOut = value;
        return true;
    }

    if ((value % 90) == 0 && value >= 0 && value <= 270) {
        orientationOut = value / 90;
        return true;
    }

    return false;
}

bool captureRegexOrientation(const QString &text, const QRegularExpression &re, int &orientationOut, bool preferLast)
{
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    bool matched = false;
    int lastOrientation = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        int candidate = 0;
        if (!normalizeRotationValue(match.captured(1), candidate)) {
            continue;
        }
        if (!preferLast) {
            orientationOut = candidate;
            return true;
        }
        lastOrientation = candidate;
        matched = true;
    }

    if (matched) {
        orientationOut = lastOrientation;
    }
    return matched;
}

bool parseWindowDisplaysOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression currentRotationRe(
        R"(mCurrentRotation\s*=\s*ROTATION_([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, currentRotationRe, orientationOut, true)) {
        return true;
    }

    static const QRegularExpression displayFramesRe(
        R"(DisplayFrames[^\n]*\br\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, displayFramesRe, orientationOut, true)) {
        return true;
    }

    static const QRegularExpression rotationRe(
        R"(\bmRotation\s*=\s*(?:ROTATION_)?([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    return captureRegexOrientation(text, rotationRe, orientationOut, true);
}

bool parseDisplayOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression currentOrientationRe(
        R"(mCurrentOrientation\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, currentOrientationRe, orientationOut, false)) {
        return true;
    }

    static const QRegularExpression displayInfoRotationRe(
        R"(DisplayDeviceInfo\{".*?",.*?\brotation\s+([0-9]{1,3}),\s+type\s+INTERNAL)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    return captureRegexOrientation(text, displayInfoRotationRe, orientationOut, false);
}

bool parseInputOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression viewportOrientationRe(
        R"(Viewport\s+INTERNAL:[^\n]*\borientation\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, viewportOrientationRe, orientationOut, false)) {
        return true;
    }

    static const QRegularExpression surfaceOrientationRe(
        R"(SurfaceOrientation\s*:\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    return captureRegexOrientation(text, surfaceOrientationRe, orientationOut, true);
}

bool runAdbShellCommand(const QString &adbPath, const QString &serial, const QStringList &shellArgs,
                        int startTimeoutMs, int finishTimeoutMs, QString &outputOut)
{
    QStringList args;
    if (!serial.trimmed().isEmpty()) {
        args << "-s" << serial;
    }
    args << "shell";
    args << shellArgs;

    QProcess process;
    process.start(adbPath, args);
    if (!process.waitForStarted(startTimeoutMs)) {
        return false;
    }
    if (!process.waitForFinished(finishTimeoutMs)) {
        process.kill();
        process.waitForFinished(200);
        return false;
    }

    outputOut = QString::fromUtf8(process.readAllStandardOutput())
                + QString::fromUtf8(process.readAllStandardError());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}
} // namespace

bool Server::parseWmSize(const QString &wmOutput, QSize &sizeOut)
{
    QRegularExpression physicalSizeRe(R"(Physical size:\s*(\d+)\s*x\s*(\d+))",
                                      QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch physicalMatch = physicalSizeRe.match(wmOutput);
    if (physicalMatch.hasMatch()) {
        const int w = physicalMatch.captured(1).toInt();
        const int h = physicalMatch.captured(2).toInt();
        if (w > 0 && h > 0) {
            sizeOut = QSize(w, h);
            return true;
        }
    }

    QRegularExpression genericSizeRe(R"((\d+)\s*x\s*(\d+))",
                                     QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch genericMatch = genericSizeRe.match(wmOutput);
    if (genericMatch.hasMatch()) {
        const int w = genericMatch.captured(1).toInt();
        const int h = genericMatch.captured(2).toInt();
        if (w > 0 && h > 0) {
            sizeOut = QSize(w, h);
            return true;
        }
    }

    return false;
}

bool Server::parseSurfaceOrientation(const QString &text, int &orientationOut)
{
    return parseWindowDisplaysOrientation(text, orientationOut)
        || parseDisplayOrientation(text, orientationOut)
        || parseInputOrientation(text, orientationOut);
}

int Server::resolveCurrentSurfaceOrientationByDumpsys(const QString &serial, bool *ok)
{
    if (ok) {
        *ok = false;
    }

    QString adbPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));
    if (adbPath.trimmed().isEmpty()) {
        adbPath = "adb";
    }

    const QList<QStringList> probeCommands = {
        QStringList{ "dumpsys", "window", "displays" },
        QStringList{ "dumpsys", "display" },
        QStringList{ "dumpsys", "input" },
    };

    for (const QStringList &shellArgs : probeCommands) {
        QString output;
        if (!runAdbShellCommand(adbPath, serial, shellArgs, 600, 1800, output)) {
            continue;
        }

        int orientation = 0;
        if (!parseSurfaceOrientation(output, orientation)) {
            continue;
        }

        if (ok) {
            *ok = true;
        }
        return orientation;
    }

    return 0;
}

QSize Server::resolveScreenSizeByWm(const QString &serial, const QSize &fallbackSize, bool *fromWm)
{
    if (fromWm) {
        *fromWm = false;
    }
    QString adbPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));
    if (adbPath.trimmed().isEmpty()) {
        adbPath = "adb";
    }
    QStringList args;
    if (!serial.trimmed().isEmpty()) {
        args << "-s" << serial;
    }
    args << "shell" << "wm" << "size";
    QProcess process;
    process.start(adbPath, args);
    if (!process.waitForStarted(500)) {
        return fallbackSize;
    }
    if (!process.waitForFinished(1200)) {
        process.kill();
        process.waitForFinished(200);
        return fallbackSize;
    }
    const QString output = QString::fromUtf8(process.readAllStandardOutput())
                               + QString::fromUtf8(process.readAllStandardError());
    QSize parsedSize;
    if (parseWmSize(output, parsedSize)) {
        if (fromWm) {
            *fromWm = true;
        }
        return parsedSize;
    }
    return fallbackSize;
}
QSize Server::resolveNoVideoDeviceSize()
{
    const QSize fallbackSize(1080, 1920);
    return resolveScreenSizeByWm(m_params.serial, fallbackSize, nullptr);
}
void Server::loadRuntimeTuningFromUserData()
{
    const QString iniPath = resolveUserDataIniPath();
    QSettings settings(iniPath, QSettings::IniFormat);
    const QString serial = m_params.serial.trimmed();
    auto readIntWithSerialOverride = [&](const QString &key, int defaultValue) -> int {
        bool ok = false;
        if (!serial.isEmpty()) {
            const QString serialPath = QString("%1/%2").arg(serial).arg(key);
            const QVariant serialValue = settings.value(serialPath);
            if (serialValue.isValid()) {
                const int parsed = serialValue.toInt(&ok);
                if (ok) {
                    return parsed;
                }
            }
        }
        const int parsed = settings.value(QString("common/%1").arg(key), defaultValue).toInt(&ok);
        return ok ? parsed : defaultValue;
    };
    m_controlMapToScreen = false;
    m_controlReferenceSize = QSize();
    m_initialOrientation = -1;

    m_params.maxFps = static_cast<quint32>(qBound(0,
                                                  readIntWithSerialOverride("MaxFps",
                                                                            static_cast<int>(m_params.maxFps)),
                                                  240));
    const QString codecStr = settings.value("common/CodecName", "").toString();
    if (!codecStr.isEmpty()) {
        m_params.codecName = codecStr;
    }
    m_audioEnabled = settings.value("common/AudioEnable", false).toBool();
    m_videoEnabled = settings.value("common/VideoEnabled", true).toBool();

    m_params.crop.clear();
    if (m_videoEnabled) {
        int cropSize = 0;
        if (!serial.isEmpty()) {
            bool cropOk = false;
            const QVariant serialCropValue = settings.value(QString("%1/%2").arg(serial, "VideoCenterCropSize"));
            if (serialCropValue.isValid()) {
                cropSize = serialCropValue.toInt(&cropOk);
                if (!cropOk || cropSize <= 0) {
                    cropSize = 0;
                }
            }
        }
        if (cropSize > 0) {
            int fallbackW = readIntWithSerialOverride("VideoCenterCropFallbackWidth", 2400);
            int fallbackH = readIntWithSerialOverride("VideoCenterCropFallbackHeight", 1080);
            if (fallbackW <= 0) {
                fallbackW = 2400;
            }
            if (fallbackH <= 0) {
                fallbackH = 1080;
            }
            bool fromWm = false;
            const QSize screenSize = resolveScreenSizeByWm(m_params.serial, QSize(fallbackW, fallbackH), &fromWm);
            const int screenW = qMax(0, screenSize.width());
            const int screenH = qMax(0, screenSize.height());
            int cropW = qMin(cropSize, screenW);
            int cropH = qMin(cropSize, screenH);
            cropW = cropW & ~1;
            cropH = cropH & ~1;
            if (cropW >= 2 && cropH >= 2) {
                const int maxX = qMax(0, screenW - cropW);
                const int maxY = qMax(0, screenH - cropH);
                int x = (screenW - cropW) / 2;
                int y = (screenH - cropH) / 2;
                x = qBound(0, x, maxX);
                y = qBound(0, y, maxY);
                x = x & ~1;
                y = y & ~1;
                x = qBound(0, x, maxX);
                y = qBound(0, y, maxY);
                m_params.crop = QString("%1:%2:%3:%4").arg(cropW).arg(cropH).arg(x).arg(y);

                if (screenW > 0 && screenH > 0) {
                    m_controlMapToScreen = true;
                    int orientedW = screenW;
                    int orientedH = screenH;
                    bool orientationOk = false;
                    const int orientation = resolveCurrentSurfaceOrientationByDumpsys(m_params.serial, &orientationOk);
                    if (orientationOk) {
                        m_initialOrientation = orientation;
                    }
                    if (orientationOk && (orientation % 2) == 1) {
                        qSwap(orientedW, orientedH);
                    }
                    m_controlReferenceSize = QSize(orientedW, orientedH);
                    qInfo() << "Control map-to-screen reference:"
                            << m_controlReferenceSize.width() << "x" << m_controlReferenceSize.height()
                            << "orientation=" << (orientationOk ? QString::number(orientation) : QString("unknown"))
                            << "initialOrientation=" << m_initialOrientation;
                }

                qInfo() << "Center crop enabled:" << m_params.crop
                        << "source=" << (fromWm ? "wm" : "fallback")
                        << "screen=" << screenW << "x" << screenH
                        << "mapToScreen=" << m_controlMapToScreen;
            } else {
                qWarning() << "Center crop disabled due to invalid size:"
                           << "cropSize=" << cropSize
                           << "screen=" << screenW << "x" << screenH;
            }
        } else {
            qInfo() << "Center crop disabled: VideoCenterCropSize<=0";
        }
    } else {
        qInfo() << "Center crop ignored: video disabled";
    }

    m_deviceName = m_params.serial;
    if (m_videoEnabled) {
        m_deviceSize = QSize();
    } else {
        m_deviceSize = resolveNoVideoDeviceSize();
    }
}
Server::Server(QObject *parent) : QObject(parent)
{
    connect(&m_workProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);
    connect(&m_serverProcess, &qsc::AdbProcess::adbProcessResult, this, &Server::onWorkProcessResult);

    connect(&m_serverSocket, &QTcpServer::newConnection, this, [this]() {
        QTcpSocket *tmp = m_serverSocket.nextPendingConnection();
        if (m_videoEnabled) {
            if (dynamic_cast<VideoSocket *>(tmp)) {
                m_videoSocket = dynamic_cast<VideoSocket *>(tmp);
                if (!m_videoSocket->isValid() || !readInfo(m_videoSocket, m_deviceName, m_deviceSize)) {
                    stop();
                    emit serverStarted(false, "", QSize(), -1);
                }
            } else {
                m_controlSocket = tmp;
                if (m_controlSocket && m_controlSocket->isValid()) {
                    // we don't need the server socket anymore
                    // just m_videoSocket is ok
                    m_serverSocket.close();
                    // we don't need the adb tunnel anymore
                    disableTunnelReverse();
                    m_tunnelEnabled = false;
                    emit serverStarted(true, m_deviceName, m_deviceSize, m_initialOrientation);
                } else {
                    stop();
                    emit serverStarted(false, "", QSize(), -1);
                }
                stopAcceptTimeoutTimer();
            }
        } else {
            m_controlSocket = tmp;
            if (m_controlSocket && m_controlSocket->isValid()) {
                m_serverSocket.close();
                disableTunnelReverse();
                m_tunnelEnabled = false;
                emit serverStarted(true, m_deviceName, m_deviceSize, m_initialOrientation);
            } else {
                stop();
                emit serverStarted(false, "", QSize(), -1);
            }
            stopAcceptTimeoutTimer();
        }
    });
}

Server::~Server() {}

bool Server::pushServer()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.push(m_params.serial, m_params.serverLocalPath, m_params.serverRemotePath);
    return true;
}

bool Server::enableTunnelReverse()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.reverse(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')), m_params.localPort);
    return true;
}

bool Server::disableTunnelReverse()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->reverseRemove(m_params.serial, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}

bool Server::enableTunnelForward()
{
    if (m_workProcess.isRuning()) {
        m_workProcess.kill();
    }
    m_workProcess.forward(m_params.serial, m_params.localPort, QString(SOCKET_NAME_PREFIX "_%1").arg(m_params.scid, 8, 16, QChar('0')));
    return true;
}
bool Server::disableTunnelForward()
{
    qsc::AdbProcess *adb = new qsc::AdbProcess();
    if (!adb) {
        return false;
    }
    connect(adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
            sender()->deleteLater();
        }
    });
    adb->forwardRemove(m_params.serial, m_params.localPort);
    return true;
}

bool Server::execute()
{
    if (m_serverProcess.isRuning()) {
        m_serverProcess.kill();
    }

    // 娴?userdata.ini 閺冧浇鐭剧拠璇插絿闂呮劘妫岄惃鍕暩缁旂偛寮弫?
    loadRuntimeTuningFromUserData();

    QStringList args;
    args << "shell";
    args << QString("CLASSPATH=%1").arg(m_params.serverRemotePath);
    args << "app_process";

#ifdef SERVER_DEBUGGER
#define SERVER_DEBUGGER_PORT "5005"

    args <<
#ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
#else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
#endif
        SERVER_DEBUGGER_PORT,
#endif

    args << "/"; // unused;
    args << "com.genymobile.scrcpy.Server";
    args << m_params.serverVersion;
    if (!m_videoEnabled) {
        args << "video=false";
    }
    args << QString("control_map_to_screen=%1").arg(m_controlMapToScreen ? "true" : "false");

    args << QString("video_bit_rate=%1").arg(QString::number(m_params.bitRate));
    if (!m_params.logLevel.isEmpty()) {
        args << QString("log_level=%1").arg(m_params.logLevel);
    }
    if (m_params.maxSize > 0) {
        args << QString("max_size=%1").arg(QString::number(m_params.maxSize));
    }
    if (m_params.maxFps > 0) {
        args << QString("max_fps=%1").arg(QString::number(m_params.maxFps));
    }

    // capture_orientation=@90
    // 閺堝牼鐞涖劎銇氶柨浣哥暰閿涘本鐥匑娑撳秹鏀ｇ€?
    // 閺堝鈧壈銆冪粈鐑樺瘹鐎规碍鏌熼崥鎴礉濞屸€斥偓鑹般€冪粈鍝勫斧婵鏌熼崥?
    if (1 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@%1").arg(m_params.captureOrientation);
    } else if (2 == m_params.captureOrientationLock) {
        args << QString("capture_orientation=@");
    } else if (0 != m_params.captureOrientation) {
        args << QString("capture_orientation=%1").arg(m_params.captureOrientation);
    }
    if (m_tunnelForward) {
        args << QString("tunnel_forward=true");
    }
    if (!m_params.crop.isEmpty()) {
        args << QString("crop=%1").arg(m_params.crop);
    }
    if (!m_params.control) {
        args << QString("control=false");
    }
    // 姒涙顓婚弰?閿涘奔绗夐棁鈧憰浣筋啎缂?
    // args << "display_id=0";
    // 姒涙顓婚弰鐥渁lse閿涘奔绗夐棁鈧憰浣筋啎缂?
    // args << "show_touches=false";
    if (m_params.stayAwake) {
        args << QString("stay_awake=true");
    }
    // code option
    // https://github.com/Genymobile/scrcpy/commit/080a4ee3654a9b7e96c8ffe37474b5c21c02852a
    // <https://d.android.com/reference/android/media/MediaFormat>
    if (!m_params.codecOptions.isEmpty()) {
        args << QString("codec_options=%1").arg(m_params.codecOptions);
    }
    if (!m_params.codecName.isEmpty()) {
        args << QString("encoder_name=%1").arg(m_params.codecName);
    }
    
    // 3. 鐠囪褰囬崢鐔烘晸闂婃娊顣跺鈧崗鐐解偓鍌氼洤閺嬫粈绗夋繅顐︾帛鐠併倕鍙ч梻?(false)
    if (!m_audioEnabled) {
        // 婵″倹鐏夊鈧崗铏梾瀵偓閿涘苯姘ㄥ楦款攽閸氭垶澧滈張铏诡伂閸欐垿鈧?audio=false 閸忔娊妫撮棅鎶筋暥
        args << "audio=false";
    }
    // 濞夘煉绱版俊鍌涚亯 enableAudio 娑?true閿涘苯鍨稉宥呭絺闁浇顕氶崣鍌涙殶閵?
    // 鎼存洖鐪?scrcpy-server 娴兼岸绮拋銈呯磻閸氼垶鐓舵０鎴炵ウ閿涘苯鐨㈤崗鏈电瑢鐟欏棝顣跺ù浣风鐠ч攱澧﹂崠鍛絺闁胶绮伴悽浣冨壋閵?
    
    // 閺堝秴濮熺粩顖炵帛鐠?1閿涘苯褰叉稉宥勭炊
    if (-1 != m_params.scid) {
        args << QString("scid=%1").arg(m_params.scid, 8, 16, QChar('0'));
    }

    // 姒涙顓婚弰鐥渁lse閿涘奔绗夐棁鈧憰浣筋啎缂?
    // args << "power_off_on_close=false";

    // 娑撳娼伴惃鍕棘閺佷即鍏橀悽銊︽箛閸旓紕顏妯款吇閸婄厧宓嗛崣顖ょ礉鐏忎粙鍣洪崙蹇撶毌閸欏倹鏆熸导鐘烩偓鎺炵礉娴肩姴寮径顏堟毐鐎佃壈鍤ф稉澶嬫Е閹靛婧€閹躲儵鏁婇敍姝磘ack corruption detected (-fstack-protector)
    /*
    args << "clipboard_autosync=true";    
    args << "downsize_on_error=true";
    args << "cleanup=true";
    args << "power_on=true";
    
    args << "send_device_meta=true";
    args << "send_frame_meta=true";
    args << "send_dummy_byte=true";
    args << "raw_video_stream=false";
    */

#ifdef SERVER_DEBUGGER
    qInfo("Server debugger waiting for a client on device port " SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif

    // adb -s P7C0218510000537 shell CLASSPATH=/data/local/tmp/scrcpy-server app_process / com.genymobile.scrcpy.Server 0 8000000 false
    // mark: crop input format: "width:height:x:y" or "" for no crop, for example: "100:200:0:0"
    // 鏉╂瑦娼痑db閸涙垝鎶ら弰顖炴▎婵夌偠绻嶇悰宀€娈戦敍瀹甠serverProcess鏉╂稓鈻兼稉宥勭窗闁偓閸戣桨绨?
    m_serverProcess.execute(m_params.serial, args);
    return true;
}

bool Server::start(Server::ServerParams params)
{
    m_params = params;
    loadRuntimeTuningFromUserData();
    m_serverSocket.setVideoSocketFirst(m_videoEnabled);
    m_serverStartStep = SSS_PUSH;
    return startServerByStep();
}

bool Server::connectTo()
{
    if (SSS_RUNNING != m_serverStartStep) {
        qWarning("server not run");
        return false;
    }

    if (!m_tunnelForward && !m_videoSocket) {
        startAcceptTimeoutTimer();
        return true;
    }

    startConnectTimeoutTimer();
    return true;
}

bool Server::isReverse()
{
    return !m_tunnelForward;
}

bool Server::isVideoEnabled() const
{
    return m_videoEnabled;
}

bool Server::isControlMapToScreenEnabled() const
{
    return m_controlMapToScreen;
}

QSize Server::getControlReferenceSize() const
{
    return m_controlReferenceSize;
}

Server::ServerParams Server::getParams()
{
    return m_params;
}

void Server::timerEvent(QTimerEvent *event)
{
    if (event && m_acceptTimeoutTimer == event->timerId()) {
        stopAcceptTimeoutTimer();
        emit serverStarted(false, "", QSize(), -1);
    } else if (event && m_connectTimeoutTimer == event->timerId()) {
        onConnectTimer();
    }
}

VideoSocket* Server::removeVideoSocket()
{
    VideoSocket* socket = m_videoSocket;
    m_videoSocket = Q_NULLPTR;
    return socket;
}

QTcpSocket *Server::getControlSocket()
{
    return m_controlSocket;
}

void Server::stop()
{
    if (m_tunnelForward) {
        stopConnectTimeoutTimer();
    } else {
        stopAcceptTimeoutTimer();
    }

    if (m_controlSocket) {
        m_controlSocket->close();
        m_controlSocket->deleteLater();
        m_controlSocket = Q_NULLPTR;
    }
    if (m_videoSocket) {
        m_videoSocket->close();
        m_videoSocket->deleteLater();
        m_videoSocket = Q_NULLPTR;
    }
    // ignore failure
    m_serverProcess.kill();
    if (m_tunnelEnabled) {
        if (m_tunnelForward) {
            disableTunnelForward();
        } else {
            disableTunnelReverse();
        }
        m_tunnelForward = false;
        m_tunnelEnabled = false;
    }
    m_serverSocket.close();
}

bool Server::startServerByStep()
{
    bool stepSuccess = false;
    // push, enable tunnel et start the server
    if (SSS_NULL != m_serverStartStep) {
        switch (m_serverStartStep) {
        case SSS_PUSH:
            stepSuccess = pushServer();
            break;
        case SSS_ENABLE_TUNNEL_REVERSE:
            stepSuccess = enableTunnelReverse();
            break;
        case SSS_ENABLE_TUNNEL_FORWARD:
            stepSuccess = enableTunnelForward();
            break;
        case SSS_EXECUTE_SERVER:
            // server will connect to our server socket
            stepSuccess = execute();
            break;
        default:
            break;
        }
    }

    if (!stepSuccess) {
        emit serverStarted(false, "", QSize(), -1);
    }
    return stepSuccess;
}
bool Server::readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size)
{
    QElapsedTimer timer;
    timer.start();
    unsigned char buf[DEVICE_NAME_FIELD_LENGTH + 12];
    while (videoSocket->bytesAvailable() <= (DEVICE_NAME_FIELD_LENGTH + 12)) {
        videoSocket->waitForReadyRead(300);
        if (timer.elapsed() > 3000) {
            qInfo("readInfo timeout");
            return false;
        }
    }
    qDebug() << "readInfo wait time:" << timer.elapsed();

    qint64 len = videoSocket->read((char *)buf, sizeof(buf));
    if (len < DEVICE_NAME_FIELD_LENGTH + 12) {
        qInfo("Could not retrieve device information");
        return false;
    }
    buf[DEVICE_NAME_FIELD_LENGTH - 1] = '\0'; // in case the client sends garbage
    deviceName = QString::fromUtf8((const char *)buf);

    // 鍓?4 涓瓧鑺傛槸 AVCodecID锛屽綋鍓嶅彧鏀寔 H264锛屾墍浠ュ厛涓嶈В鏋?    size.setWidth(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 4]));
    size.setHeight(bufferRead32be(&buf[DEVICE_NAME_FIELD_LENGTH + 8]));

    return true;
}
void Server::startAcceptTimeoutTimer()
{
    stopAcceptTimeoutTimer();
    m_acceptTimeoutTimer = startTimer(1000);
}

void Server::stopAcceptTimeoutTimer()
{
    if (m_acceptTimeoutTimer) {
        killTimer(m_acceptTimeoutTimer);
        m_acceptTimeoutTimer = 0;
    }
}

void Server::startConnectTimeoutTimer()
{
    stopConnectTimeoutTimer();
    m_connectTimeoutTimer = startTimer(300);
}

void Server::stopConnectTimeoutTimer()
{
    if (m_connectTimeoutTimer) {
        killTimer(m_connectTimeoutTimer);
        m_connectTimeoutTimer = 0;
    }
    m_connectCount = 0;
}

void Server::onConnectTimer()
{
    QString deviceName = m_deviceName;
    QSize deviceSize = m_deviceSize;
    bool success = false;

    VideoSocket *videoSocket = Q_NULLPTR;
    QTcpSocket *controlSocket = new QTcpSocket();

    if (m_videoEnabled) {
        videoSocket = new VideoSocket();
        videoSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
        if (!videoSocket->waitForConnected(1000)) {
            m_connectCount = MAX_CONNECT_COUNT;
            qWarning("video socket connect to server failed");
            goto result;
        }

        controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
        if (!controlSocket->waitForConnected(1000)) {
            m_connectCount = MAX_CONNECT_COUNT;
            qWarning("control socket connect to server failed");
            goto result;
        }

        if (QTcpSocket::ConnectedState == videoSocket->state()) {
            videoSocket->waitForReadyRead(1000);
            QByteArray data = videoSocket->read(1);
            if (!data.isEmpty() && readInfo(videoSocket, deviceName, deviceSize)) {
                success = true;
                goto result;
            } else {
                qWarning("video socket connect to server read device info failed, try again");
                goto result;
            }
        } else {
            qWarning("connect to server failed");
            m_connectCount = MAX_CONNECT_COUNT;
            goto result;
        }
    } else {
        controlSocket->connectToHost(QHostAddress::LocalHost, m_params.localPort);
        if (!controlSocket->waitForConnected(1000)) {
            m_connectCount = MAX_CONNECT_COUNT;
            qWarning("control socket (no-video) connect to server failed");
            goto result;
        }
        if (!controlSocket->waitForReadyRead(10)) {
            // no-op: control socket may not have immediate data
        }
        if (!controlSocket->isValid()) {
            m_connectCount = MAX_CONNECT_COUNT;
            qWarning("control socket (no-video) invalid");
            goto result;
        }
        if (!deviceSize.isValid()) {
            deviceSize = resolveNoVideoDeviceSize();
        }
        if (deviceName.trimmed().isEmpty()) {
            deviceName = m_params.serial;
        }
        success = true;
    }

result:
    if (success) {
        stopConnectTimeoutTimer();
        if (videoSocket) {
            m_videoSocket = videoSocket;
        }
        if (controlSocket->bytesAvailable() > 0) {
            controlSocket->read(1);
        }
        m_controlSocket = controlSocket;
        disableTunnelForward();
        m_tunnelEnabled = false;
        m_restartCount = 0;
        emit serverStarted(true, deviceName, deviceSize, m_initialOrientation);
        return;
    }

    if (videoSocket) {
        videoSocket->deleteLater();
    }
    if (controlSocket) {
        controlSocket->deleteLater();
    }

    if (MAX_CONNECT_COUNT <= m_connectCount++) {
        stopConnectTimeoutTimer();
        stop();
        if (MAX_RESTART_COUNT > m_restartCount++) {
            qWarning("restart server auto");
            start(m_params);
        } else {
            m_restartCount = 0;
            emit serverStarted(false, "", QSize(), -1);
        }
    }
}

void Server::onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult)
{
    if (sender() == &m_workProcess) {
        if (SSS_NULL != m_serverStartStep) {
            switch (m_serverStartStep) {
            case SSS_PUSH:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    if (m_params.useReverse) {
                        m_serverStartStep = SSS_ENABLE_TUNNEL_REVERSE;
                    } else {
                        m_tunnelForward = true;
                        m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    }
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb push failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false, "", QSize(), -1);
                }
                break;
            case SSS_ENABLE_TUNNEL_REVERSE:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    // At the application level, the device part is "the server" because it
                    // serves video stream and control. However, at the network level, the
                    // client listens and the server connects to the client. That way, the
                    // client can listen before starting the server app, so there is no need to
                    // try to connect until the server socket is listening on the device.
                    m_serverSocket.setVideoSocketFirst(m_videoEnabled);
                    m_serverSocket.setMaxPendingConnections(m_videoEnabled ? 2 : 1);
                    if (!m_serverSocket.listen(QHostAddress::LocalHost, m_params.localPort)) {
                        qCritical() << QString("Could not listen on port %1").arg(m_params.localPort).toStdString().c_str();
                        m_serverStartStep = SSS_NULL;
                        disableTunnelReverse();
                        emit serverStarted(false, "", QSize(), -1);
                        break;
                    }

                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    // 閺堝绔存禍娑滎啎婢跺檺everse娴兼碍濮ら柨妾搊re than o'ne device閿涘畮db閻ㄥ垺ug
                    // https://github.com/Genymobile/scrcpy/issues/5
                    qCritical("adb reverse failed");
                    m_tunnelForward = true;
                    m_serverStartStep = SSS_ENABLE_TUNNEL_FORWARD;
                    startServerByStep();
                }
                break;
            case SSS_ENABLE_TUNNEL_FORWARD:
                if (qsc::AdbProcess::AER_SUCCESS_EXEC == processResult) {
                    m_serverStartStep = SSS_EXECUTE_SERVER;
                    startServerByStep();
                } else if (qsc::AdbProcess::AER_SUCCESS_START != processResult) {
                    qCritical("adb forward failed");
                    m_serverStartStep = SSS_NULL;
                    emit serverStarted(false, "", QSize(), -1);
                }
                break;
            default:
                break;
            }
        }
    }
    if (sender() == &m_serverProcess) {
        if (SSS_EXECUTE_SERVER == m_serverStartStep) {
            if (qsc::AdbProcess::AER_SUCCESS_START == processResult) {
                m_serverStartStep = SSS_RUNNING;
                m_tunnelEnabled = true;
                connectTo();
            } else if (qsc::AdbProcess::AER_ERROR_START == processResult) {
                if (!m_tunnelForward) {
                    m_serverSocket.close();
                    disableTunnelReverse();
                } else {
                    disableTunnelForward();
                }
                qCritical("adb shell start server failed");
                m_serverStartStep = SSS_NULL;
                emit serverStarted(false, "", QSize(), -1);
            }
        } else if (SSS_RUNNING == m_serverStartStep) {
            m_serverStartStep = SSS_NULL;
            emit serverStoped();
        }
    }
}













