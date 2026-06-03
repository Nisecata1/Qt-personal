#ifndef SERVER_H
#define SERVER_H

#include <QObject>
#include <QPointer>
#include <QSize>

#include "adbprocess.h"
#include "tcpserver.h"
#include "videosocket.h"

class Server : public QObject
{
    Q_OBJECT

    enum SERVER_START_STEP
    {
        SSS_NULL,
        SSS_PUSH,
        SSS_ENABLE_TUNNEL_REVERSE,
        SSS_ENABLE_TUNNEL_FORWARD,
        SSS_EXECUTE_SERVER,
        SSS_RUNNING,
    };

public:
    struct ServerParams
    {
        // necessary
        QString serial = "";              // 璁惧搴忓垪鍙?
        QString serverLocalPath = "";     // 鏈湴瀹夊崜server璺緞

        // optional
        QString serverRemotePath = "/data/local/tmp/scrcpy-server.jar";    // 瑕佹帹閫佸埌杩滅璁惧鐨剆erver璺緞
        quint16 localPort = 27183;     // reverse鏃舵湰鍦扮洃鍚鍙?
        quint16 maxSize = 720;         // 瑙嗛鍒嗚鲸鐜?
        quint32 bitRate = 8000000;     // 瑙嗛姣旂壒鐜?
        quint32 maxFps = 0;            // 瑙嗛鏈€澶у抚鐜?
        bool useReverse = true;        // true:鍏堜娇鐢╝db reverse锛屽け璐ュ悗鑷姩浣跨敤adb forward锛沠alse:鐩存帴浣跨敤adb forward
        int captureOrientationLock = 0; // 鏄惁閿佸畾閲囬泦鏂瑰悜 0涓嶉攣瀹?1閿佸畾鎸囧畾鏂瑰悜 2閿佸畾鍘熷鏂瑰悜
        int captureOrientation = 0;     // 閲囬泦鏂瑰悜 0 90 180 270
        int stayAwake = false;         // 鏄惁淇濇寔鍞ら啋
        QString serverVersion = "3.3.3"; // server鐗堟湰
        QString logLevel = "debug";  // log绾у埆 verbose/debug/info/warn/error
        // 缂栫爜閫夐」 ""琛ㄧず榛樿
        // 渚嬪 CodecOptions="profile=1,level=2"
        // 鏇村缂栫爜閫夐」鍙傝€?https://d.android.com/reference/android/media/MediaFormat
        QString codecOptions = "";
        // 鎸囧畾缂栫爜鍣ㄥ悕绉?蹇呴』鏄疕.264缂栫爜鍣?锛?"琛ㄧず榛樿
        // 渚嬪 CodecName="OMX.qcom.video.encoder.avc"
        QString codecName = "";

        QString crop = "";             // 瑙嗛瑁佸壀
        bool control = true;           // 瀹夊崜绔槸鍚︽帴鏀堕敭榧犳帶鍒?
        qint32 scid = -1;             // 闅忔満鏁帮紝浣滀负localsocket鍚嶅瓧鍚庣紑锛屾柟渚垮悓鏃惰繛鎺ュ悓涓€涓澶囧娆?
    };

    explicit Server(QObject *parent = nullptr);
    virtual ~Server();

    bool start(Server::ServerParams params);
    void stop();
    bool isReverse();
    bool isVideoEnabled() const;
    bool isControlMapToScreenEnabled() const;
    QSize getControlReferenceSize() const;
    Server::ServerParams getParams();
    VideoSocket *removeVideoSocket();
    QTcpSocket *getControlSocket();

signals:
    void serverStarted(bool success, const QString &deviceName = "", const QSize &size = QSize(), int initialOrientation = -1);
    void serverStoped();

private slots:
    void onWorkProcessResult(qsc::AdbProcess::ADB_EXEC_RESULT processResult);

protected:
    void timerEvent(QTimerEvent *event);

private:
    bool pushServer();
    bool enableTunnelReverse();
    bool disableTunnelReverse();
    bool enableTunnelForward();
    bool disableTunnelForward();
    bool execute();
    bool connectTo();
    bool startServerByStep();
    bool readInfo(VideoSocket *videoSocket, QString &deviceName, QSize &size);
    void startAcceptTimeoutTimer();
    void stopAcceptTimeoutTimer();
    void startConnectTimeoutTimer();
    void stopConnectTimeoutTimer();
    void onConnectTimer();

private:
    void loadRuntimeTuningFromUserData();
    QSize resolveScreenSizeByWm(const QString &serial, const QSize &fallbackSize, bool *fromWm = nullptr);
    int resolveCurrentSurfaceOrientationByDumpsys(const QString &serial, bool *ok = nullptr);
    QSize resolveNoVideoDeviceSize();
    static bool parseWmSize(const QString &wmOutput, QSize &sizeOut);
    static bool parseSurfaceOrientation(const QString &text, int &orientationOut);

private:
    qsc::AdbProcess m_workProcess;
    qsc::AdbProcess m_serverProcess;
    TcpServer m_serverSocket; // only used if !tunnel_forward
    QPointer<VideoSocket> m_videoSocket = Q_NULLPTR;
    QPointer<QTcpSocket> m_controlSocket = Q_NULLPTR;
    bool m_tunnelEnabled = false;
    bool m_tunnelForward = false; // use "adb forward" instead of "adb reverse"
    int m_acceptTimeoutTimer = 0;
    int m_connectTimeoutTimer = 0;
    quint32 m_connectCount = 0;
    quint32 m_restartCount = 0;
    QString m_deviceName = "";
    QSize m_deviceSize = QSize();
    bool m_videoEnabled = true;
    bool m_controlMapToScreen = false;
    QSize m_controlReferenceSize = QSize();
    int m_initialOrientation = -1;
    bool m_audioEnabled = false;
    ServerParams m_params;

    SERVER_START_STEP m_serverStartStep = SSS_NULL;
};

#endif // SERVER_H





