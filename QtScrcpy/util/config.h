#ifndef CONFIG_H
#define CONFIG_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QRect>

enum class ThemeMode
{
    System = 0,
    Light,
    Dark,
};

struct UserBootConfig
{
    QString recordPath = "";
    quint32 bitRate = 2000000;
    ThemeMode themeMode = ThemeMode::System;
    int maxFps = 0;
    int maxSizeIndex = 0;
    int recordFormatIndex = 0;
    int lockOrientationIndex = 0;
    bool localTextInputEnabled = true;
    QString localTextInputShortcut = "Ctrl+Shift+T";
    QString keymapEditorShortcut = "Ctrl+E";
    bool recordScreen     = false;
    bool recordBackground = false;
    bool reverseConnect   = true;
    bool showFPS          = false;
    bool windowOnTop      = false;
    bool autoOffScreen    = false;
    bool framelessWindow  = false;
    bool keepAlive        = false;
    bool simpleMode       = false;
    bool autoUpdateDevice = true;
    int autoUpdateIntervalSec = 5;
    bool showToolbar      = true;
};

struct DeviceMouseConfig
{
    bool remoteCursorEnabled = false;
    int cursorSizePx = 24;
    bool normalMouseCompatEnabled = false;
    bool normalMouseTouchPriorityEnabled = true;
    bool normalMouseCursorThrottleEnabled = true;
    int normalMouseCursorFlushIntervalMs = 33;
    int normalMouseCursorClickSuppressionMs = 120;
    int normalMouseTapMinHoldMs = 16;
};

class QSettings;
class Config : public QObject
{
    Q_OBJECT
public:

    static Config &getInstance();

    // config
    QString getLanguage();
    QString getTitle();
    QString getStartupConsoleText();
    int getGlobalMaxFps();
    int getMaxFps();
    int getDesktopOpenGL();
    int getSkin();
    int getRenderExpiredFrames();
    QString getPushFilePath();
    QString getServerPath();
    QString getAdbPath();
    QString getLogLevel();
    QString getCodecOptions();
    QString getCodecName();
    QStringList getConnectedGroups();

    // user data:common
    void setUserBootConfig(const UserBootConfig &config);
    UserBootConfig getUserBootConfig();
    void setTrayMessageShown(bool shown);
    bool getTrayMessageShown();

    // user data:device
    void setNickName(const QString &serial, const QString &name);
    QString getNickName(const QString &serial);
    void setRect(const QString &serial, const QRect &rc);
    QRect getRect(const QString &serial);
    void setKeymapEditorRect(const QString &serial, const QRect &rc);
    QRect getKeymapEditorRect(const QString &serial);
    bool isDeviceCenterCropEnabled(const QString &serial);
    int getDeviceCenterCropSize(const QString &serial);
    void setDeviceCenterCropSize(const QString &serial, int cropSize);
    void clearDeviceCenterCropSize(const QString &serial);
    DeviceMouseConfig getDeviceMouseConfig(const QString &serial);
    void ensureDeviceMouseConfigInitialized(const QString &serial);
    void setDeviceMouseConfig(const QString &serial, const DeviceMouseConfig &config);

    void deleteGroup(const QString &serial);

    // IP history methods
    void saveIpHistory(const QString &ip);
    QStringList getIpHistory(); 
    void clearIpHistory();

    // Port history methods
    void savePortHistory(const QString &port);
    QStringList getPortHistory(); 
    void clearPortHistory();

private:
    explicit Config(QObject *parent = nullptr);
    const QString &getConfigPath();

private:
    static QString s_configPath;
    QPointer<QSettings> m_settings;
    QPointer<QSettings> m_userData;
};

#endif // CONFIG_H
