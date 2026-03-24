#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QDebug>
#include <QtGlobal>

#include "config.h"
#ifdef Q_OS_OSX
#include "path.h"
#endif

#define GROUP_COMMON "common"

// config
#define COMMON_LANGUAGE_KEY "Language"
#define COMMON_LANGUAGE_DEF "Auto"

#define COMMON_TITLE_KEY "WindowTitle"
#define COMMON_TITLE_DEF QCoreApplication::applicationName()

#define COMMON_STARTUP_CONSOLE_TEXT_KEY "StartupConsoleText"
#define COMMON_STARTUP_CONSOLE_TEXT_DEF ""

#define COMMON_PUSHFILE_KEY "PushFilePath"
#define COMMON_PUSHFILE_DEF "/sdcard/"

#define COMMON_SERVER_PATH_KEY "ServerPath"
#define COMMON_SERVER_PATH_DEF "/data/local/tmp/scrcpy-server.jar"

#define COMMON_MAX_FPS_KEY "MaxFps"
#define COMMON_MAX_FPS_DEF 0

#define COMMON_DESKTOP_OPENGL_KEY "UseDesktopOpenGL"
#define COMMON_DESKTOP_OPENGL_DEF -1

#define COMMON_SKIN_KEY "UseSkin"
#define COMMON_SKIN_DEF 1

#define COMMON_RENDER_EXPIRED_FRAMES_KEY "RenderExpiredFrames"
#define COMMON_RENDER_EXPIRED_FRAMES_DEF 0

#define COMMON_ADB_PATH_KEY "AdbPath"
#define COMMON_ADB_PATH_DEF ""

#define COMMON_LOG_LEVEL_KEY "LogLevel"
#define COMMON_LOG_LEVEL_DEF "info"

#define COMMON_CODEC_OPTIONS_KEY "CodecOptions"
#define COMMON_CODEC_OPTIONS_DEF ""

#define COMMON_CODEC_NAME_KEY "CodecName"
#define COMMON_CODEC_NAME_DEF ""

// user config
#define COMMON_THEME_MODE_KEY "ThemeMode"
#define COMMON_THEME_MODE_DEF "System"

#define COMMON_RECORD_KEY "RecordPath"
#define COMMON_RECORD_DEF ""

#define COMMON_BITRATE_KEY "BitRate"
#define COMMON_BITRATE_DEF 2000000

#define COMMON_MAX_SIZE_INDEX_KEY "MaxSizeIndex"
#define COMMON_MAX_SIZE_INDEX_DEF 2

#define COMMON_RECORD_FORMAT_INDEX_KEY "RecordFormatIndex"
#define COMMON_RECORD_FORMAT_INDEX_DEF 0

#define COMMON_LOCK_ORIENTATION_INDEX_KEY "LockDirectionIndex"
#define COMMON_LOCK_ORIENTATION_INDEX_DEF 0

#define COMMON_VIDEO_CENTER_CROP_SIZE_KEY "VideoCenterCropSize"
#define COMMON_VIDEO_CENTER_CROP_SIZE_DEF 0

#define COMMON_LOCAL_TEXT_INPUT_ENABLED_KEY "LocalTextInputEnabled"
#define COMMON_LOCAL_TEXT_INPUT_ENABLED_DEF true

#define COMMON_LOCAL_TEXT_INPUT_SHORTCUT_KEY "LocalTextInputShortcut"
#define COMMON_LOCAL_TEXT_INPUT_SHORTCUT_DEF "Ctrl+Shift+T"

#define COMMON_KEYMAP_EDITOR_SHORTCUT_KEY "KeymapEditorShortcut"
#define COMMON_KEYMAP_EDITOR_SHORTCUT_DEF "Ctrl+E"

#define COMMON_RECORD_SCREEN_KEY "RecordScreen"
#define COMMON_RECORD_SCREEN_DEF false

#define COMMON_RECORD_BACKGROUD_KEY "RecordBackGround"
#define COMMON_RECORD_BACKGROUD_DEF false

#define COMMON_REVERSE_CONNECT_KEY "ReverseConnect"
#define COMMON_REVERSE_CONNECT_DEF true

#define COMMON_SHOW_FPS_KEY "ShowFPS"
#define COMMON_SHOW_FPS_DEF false

#define COMMON_WINDOW_ON_TOP_KEY "WindowOnTop"
#define COMMON_WINDOW_ON_TOP_DEF false

#define COMMON_AUTO_OFF_SCREEN_KEY "AutoOffScreen"
#define COMMON_AUTO_OFF_SCREEN_DEF false

#define COMMON_FRAMELESS_WINDOW_KEY "FramelessWindow"
#define COMMON_FRAMELESS_WINDOW_DEF false

#define COMMON_KEEP_ALIVE_KEY "KeepAlive"
#define COMMON_KEEP_ALIVE_DEF false

#define COMMON_SIMPLE_MODE_KEY "SimpleMode"
#define COMMON_SIMPLE_MODE_DEF false

#define COMMON_AUTO_UPDATE_DEVICE_KEY "AutoUpdateDevice"
#define COMMON_AUTO_UPDATE_DEVICE_DEF true

#define COMMON_AUTO_UPDATE_INTERVAL_SEC_KEY "AutoUpdateIntervalSec"
#define COMMON_AUTO_UPDATE_INTERVAL_SEC_DEF 5

#define COMMON_TRAY_MESSAGE_SHOWN_KEY "TrayMessageShown"
#define COMMON_TRAY_MESSAGE_SHOWN_DEF false

#define COMMON_SHOW_TOOLBAR_KEY "showToolbar"
#define COMMON_SHOW_TOOLBAR_DEF true

// device config
#define SERIAL_WINDOW_RECT_KEY_X "WindowRectX"
#define SERIAL_WINDOW_RECT_KEY_Y "WindowRectY"
#define SERIAL_WINDOW_RECT_KEY_W "WindowRectW"
#define SERIAL_WINDOW_RECT_KEY_H "WindowRectH"
#define SERIAL_WINDOW_RECT_KEY_DEF -1
#define SERIAL_KEYMAP_EDITOR_RECT_KEY_X "KeymapEditorRectX"
#define SERIAL_KEYMAP_EDITOR_RECT_KEY_Y "KeymapEditorRectY"
#define SERIAL_KEYMAP_EDITOR_RECT_KEY_W "KeymapEditorRectW"
#define SERIAL_KEYMAP_EDITOR_RECT_KEY_H "KeymapEditorRectH"
#define SERIAL_NICK_NAME_KEY "NickName"
#define SERIAL_NICK_NAME_DEF "Phone"
#define SERIAL_REMOTE_CURSOR_ENABLED_KEY "RemoteCursorEnabled"
#define SERIAL_CURSOR_SIZE_PX_KEY "CursorSizePx"
#define SERIAL_NORMAL_MOUSE_COMPAT_ENABLED_KEY "NormalMouseCompatEnabled"
#define SERIAL_NORMAL_MOUSE_TOUCH_PRIORITY_ENABLED_KEY "NormalMouseTouchPriorityEnabled"
#define SERIAL_NORMAL_MOUSE_CURSOR_THROTTLE_ENABLED_KEY "NormalMouseCursorThrottleEnabled"
#define SERIAL_NORMAL_MOUSE_CURSOR_FLUSH_INTERVAL_MS_KEY "NormalMouseCursorFlushIntervalMs"
#define SERIAL_NORMAL_MOUSE_CURSOR_CLICK_SUPPRESSION_MS_KEY "NormalMouseCursorClickSuppressionMs"
#define SERIAL_NORMAL_MOUSE_TAP_MIN_HOLD_MS_KEY "NormalMouseTapMinHoldMs"

// IP history
#define IP_HISTORY_KEY "IpHistory"
#define IP_HISTORY_DEF ""
#define IP_HISTORY_MAX 10

// Port history  
#define PORT_HISTORY_KEY "PortHistory"
#define PORT_HISTORY_DEF ""
#define PORT_HISTORY_MAX 10

QString Config::s_configPath = "";

namespace {
bool parseBoolSetting(const QVariant &value, bool defaultValue, bool *ok = nullptr)
{
    if (ok) {
        *ok = false;
    }

    if (!value.isValid() || value.isNull()) {
        return defaultValue;
    }

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    if (value.typeId() == QMetaType::Bool) {
#else
    if (value.type() == QVariant::Bool) {
#endif
        if (ok) {
            *ok = true;
        }
        return value.toBool();
    }

    bool intOk = false;
    const int intValue = value.toInt(&intOk);
    if (intOk) {
        if (ok) {
            *ok = true;
        }
        return intValue != 0;
    }

    const QString text = value.toString().trimmed().toLower();
    if (text == "true" || text == "yes" || text == "on" || text == "1") {
        if (ok) {
            *ok = true;
        }
        return true;
    }
    if (text == "false" || text == "no" || text == "off" || text == "0") {
        if (ok) {
            *ok = true;
        }
        return false;
    }

    return defaultValue;
}

QString themeModeToString(ThemeMode mode)
{
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("Light");
    case ThemeMode::Dark:
        return QStringLiteral("Dark");
    case ThemeMode::System:
    default:
        return QStringLiteral("System");
    }
}

ThemeMode themeModeFromVariant(const QVariant &value)
{
    if (!value.isValid() || value.isNull()) {
        return ThemeMode::System;
    }

#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    if (value.typeId() == QMetaType::Int) {
#else
    if (value.type() == QVariant::Int) {
#endif
        switch (value.toInt()) {
        case static_cast<int>(ThemeMode::Light):
            return ThemeMode::Light;
        case static_cast<int>(ThemeMode::Dark):
            return ThemeMode::Dark;
        case static_cast<int>(ThemeMode::System):
        default:
            return ThemeMode::System;
        }
    }

    const QString text = value.toString().trimmed().toLower();
    if (text == QLatin1String("light")) {
        return ThemeMode::Light;
    }
    if (text == QLatin1String("dark")) {
        return ThemeMode::Dark;
    }
    return ThemeMode::System;
}

bool hasCompleteDeviceMouseConfig(QSettings *settings)
{
    return settings
        && settings->contains(SERIAL_REMOTE_CURSOR_ENABLED_KEY)
        && settings->contains(SERIAL_CURSOR_SIZE_PX_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_COMPAT_ENABLED_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_TOUCH_PRIORITY_ENABLED_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_CURSOR_THROTTLE_ENABLED_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_CURSOR_FLUSH_INTERVAL_MS_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_CURSOR_CLICK_SUPPRESSION_MS_KEY)
        && settings->contains(SERIAL_NORMAL_MOUSE_TAP_MIN_HOLD_MS_KEY);
}
}

Config::Config(QObject *parent) : QObject(parent)
{
    m_settings = new QSettings(getConfigPath() + "/config.ini", QSettings::IniFormat);
    m_userData = new QSettings(getConfigPath() + "/userdata.ini", QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    m_settings->setIniCodec("UTF-8");
    m_userData->setIniCodec("UTF-8");
#endif

    qDebug()<<m_userData->childGroups();
}

Config &Config::getInstance()
{
    static Config config;
    return config;
}

const QString &Config::getConfigPath()
{
    if (s_configPath.isEmpty()) {
        // Priority:
        // 1) "<exe-dir>/config"
        // 2) QTSCRCPY_CONFIG_PATH
        // 3) legacy fallback
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            const QString appConfigPath = appDir + "/config";
            QFileInfo appConfigInfo(appConfigPath);
            if (appConfigInfo.isDir()) {
                s_configPath = appConfigPath;
                return s_configPath;
            }
        }

        const QString envConfigPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_CONFIG_PATH"));
        QFileInfo envConfigInfo(envConfigPath);
        if (!envConfigPath.isEmpty() && envConfigInfo.isDir()) {
            s_configPath = envConfigPath;
            return s_configPath;
        }

        if (!appDir.isEmpty()) {
            // If "<exe-dir>/config" does not exist yet, still use it as the default target.
            s_configPath = appDir + "/config";
        } else {
            // mac系统当从finder打开app时，默认工作目录不再是可执行程序的目录了，而是"/"
            // 而Qt的获取工作目录的api都依赖QCoreApplication的初始化，所以使用mac api获取当前目录
#ifdef Q_OS_OSX
            // get */QtScrcpy.app path
            s_configPath = Path::GetCurrentPath();
            s_configPath += "/Contents/MacOS/config";
#else
            s_configPath = "config";
#endif
        }
    }
    return s_configPath;
}

void Config::setUserBootConfig(const UserBootConfig &config)
{
    m_userData->beginGroup(GROUP_COMMON);
    m_userData->setValue(COMMON_THEME_MODE_KEY, themeModeToString(config.themeMode));
    m_userData->setValue(COMMON_RECORD_KEY, config.recordPath);
    m_userData->setValue(COMMON_BITRATE_KEY, config.bitRate);
    m_userData->setValue(COMMON_MAX_FPS_KEY, qBound(0, config.maxFps, 240));
    m_userData->setValue(COMMON_MAX_SIZE_INDEX_KEY, config.maxSizeIndex);
    m_userData->setValue(COMMON_RECORD_FORMAT_INDEX_KEY, config.recordFormatIndex);
    m_userData->setValue(COMMON_FRAMELESS_WINDOW_KEY, config.framelessWindow);
    m_userData->setValue(COMMON_LOCK_ORIENTATION_INDEX_KEY, config.lockOrientationIndex);
    m_userData->setValue(COMMON_LOCAL_TEXT_INPUT_ENABLED_KEY, config.localTextInputEnabled);
    m_userData->setValue(COMMON_LOCAL_TEXT_INPUT_SHORTCUT_KEY, config.localTextInputShortcut);
    m_userData->setValue(COMMON_KEYMAP_EDITOR_SHORTCUT_KEY, config.keymapEditorShortcut);
    m_userData->setValue(COMMON_RECORD_SCREEN_KEY, config.recordScreen);
    m_userData->setValue(COMMON_RECORD_BACKGROUD_KEY, config.recordBackground);
    m_userData->setValue(COMMON_REVERSE_CONNECT_KEY, config.reverseConnect);
    m_userData->setValue(COMMON_SHOW_FPS_KEY, config.showFPS);
    m_userData->setValue(COMMON_WINDOW_ON_TOP_KEY, config.windowOnTop);
    m_userData->setValue(COMMON_AUTO_OFF_SCREEN_KEY, config.autoOffScreen);
    m_userData->setValue(COMMON_KEEP_ALIVE_KEY, config.keepAlive);
    m_userData->setValue(COMMON_SIMPLE_MODE_KEY, config.simpleMode);
    m_userData->setValue(COMMON_AUTO_UPDATE_DEVICE_KEY, config.autoUpdateDevice);
    m_userData->setValue(COMMON_AUTO_UPDATE_INTERVAL_SEC_KEY, qBound(1, config.autoUpdateIntervalSec, 3600));
    m_userData->setValue(COMMON_SHOW_TOOLBAR_KEY, config.showToolbar);
    m_userData->endGroup();
    m_userData->sync();
}

UserBootConfig Config::getUserBootConfig()
{
    UserBootConfig config;
    m_userData->beginGroup(GROUP_COMMON);
    config.themeMode = themeModeFromVariant(m_userData->value(COMMON_THEME_MODE_KEY, COMMON_THEME_MODE_DEF));
    config.recordPath = m_userData->value(COMMON_RECORD_KEY, COMMON_RECORD_DEF).toString();
    config.bitRate = m_userData->value(COMMON_BITRATE_KEY, COMMON_BITRATE_DEF).toUInt();
    config.maxSizeIndex = m_userData->value(COMMON_MAX_SIZE_INDEX_KEY, COMMON_MAX_SIZE_INDEX_DEF).toInt();
    config.recordFormatIndex = m_userData->value(COMMON_RECORD_FORMAT_INDEX_KEY, COMMON_RECORD_FORMAT_INDEX_DEF).toInt();
    config.lockOrientationIndex = m_userData->value(COMMON_LOCK_ORIENTATION_INDEX_KEY, COMMON_LOCK_ORIENTATION_INDEX_DEF).toInt();
    config.localTextInputEnabled = m_userData->value(COMMON_LOCAL_TEXT_INPUT_ENABLED_KEY, COMMON_LOCAL_TEXT_INPUT_ENABLED_DEF).toBool();
    config.localTextInputShortcut = m_userData->value(COMMON_LOCAL_TEXT_INPUT_SHORTCUT_KEY, COMMON_LOCAL_TEXT_INPUT_SHORTCUT_DEF).toString();
    config.keymapEditorShortcut = m_userData->value(COMMON_KEYMAP_EDITOR_SHORTCUT_KEY, COMMON_KEYMAP_EDITOR_SHORTCUT_DEF).toString();
    config.framelessWindow = m_userData->value(COMMON_FRAMELESS_WINDOW_KEY, COMMON_FRAMELESS_WINDOW_DEF).toBool();
    config.recordScreen = m_userData->value(COMMON_RECORD_SCREEN_KEY, COMMON_RECORD_SCREEN_DEF).toBool();
    config.recordBackground = m_userData->value(COMMON_RECORD_BACKGROUD_KEY, COMMON_RECORD_BACKGROUD_DEF).toBool();
    config.reverseConnect = m_userData->value(COMMON_REVERSE_CONNECT_KEY, COMMON_REVERSE_CONNECT_DEF).toBool();
    config.showFPS = m_userData->value(COMMON_SHOW_FPS_KEY, COMMON_SHOW_FPS_DEF).toBool();
    config.windowOnTop = m_userData->value(COMMON_WINDOW_ON_TOP_KEY, COMMON_WINDOW_ON_TOP_DEF).toBool();
    config.autoOffScreen = m_userData->value(COMMON_AUTO_OFF_SCREEN_KEY, COMMON_AUTO_OFF_SCREEN_DEF).toBool();
    config.keepAlive = m_userData->value(COMMON_KEEP_ALIVE_KEY, COMMON_KEEP_ALIVE_DEF).toBool();
    config.simpleMode = m_userData->value(COMMON_SIMPLE_MODE_KEY, COMMON_SIMPLE_MODE_DEF).toBool();
    config.autoUpdateDevice = m_userData->value(COMMON_AUTO_UPDATE_DEVICE_KEY, COMMON_AUTO_UPDATE_DEVICE_DEF).toBool();
    {
        bool intervalOk = false;
        const int intervalSec = m_userData->value(COMMON_AUTO_UPDATE_INTERVAL_SEC_KEY, COMMON_AUTO_UPDATE_INTERVAL_SEC_DEF).toInt(&intervalOk);
        config.autoUpdateIntervalSec = qBound(1, intervalOk ? intervalSec : COMMON_AUTO_UPDATE_INTERVAL_SEC_DEF, 3600);
    }
    config.showToolbar =m_userData->value(COMMON_SHOW_TOOLBAR_KEY,COMMON_SHOW_TOOLBAR_DEF).toBool();
    m_userData->endGroup();
    config.maxFps = getGlobalMaxFps();
    return config;
}

void Config::setTrayMessageShown(bool shown)
{
    m_userData->beginGroup(GROUP_COMMON);
    m_userData->setValue(COMMON_TRAY_MESSAGE_SHOWN_KEY, shown);
    m_userData->endGroup();
    m_userData->sync();
}

bool Config::getTrayMessageShown()
{
    bool shown;
    m_userData->beginGroup(GROUP_COMMON);
    shown = m_userData->value(COMMON_TRAY_MESSAGE_SHOWN_KEY, COMMON_TRAY_MESSAGE_SHOWN_DEF).toBool();
    m_userData->endGroup();
    return shown;
}

void Config::setRect(const QString &serial, const QRect &rc)
{
    m_userData->beginGroup(serial);
    m_userData->setValue(SERIAL_WINDOW_RECT_KEY_X, rc.left());
    m_userData->setValue(SERIAL_WINDOW_RECT_KEY_Y, rc.top());
    m_userData->setValue(SERIAL_WINDOW_RECT_KEY_W, rc.width());
    m_userData->setValue(SERIAL_WINDOW_RECT_KEY_H, rc.height());
    m_userData->endGroup();
    m_userData->sync();
}

QRect Config::getRect(const QString &serial)
{
    QRect rc;
    m_userData->beginGroup(serial);
    rc.setX(m_userData->value(SERIAL_WINDOW_RECT_KEY_X, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setY(m_userData->value(SERIAL_WINDOW_RECT_KEY_Y, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setWidth(m_userData->value(SERIAL_WINDOW_RECT_KEY_W, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setHeight(m_userData->value(SERIAL_WINDOW_RECT_KEY_H, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    m_userData->endGroup();
    return rc;
}

void Config::setKeymapEditorRect(const QString &serial, const QRect &rc)
{
    m_userData->beginGroup(serial);
    m_userData->setValue(SERIAL_KEYMAP_EDITOR_RECT_KEY_X, rc.left());
    m_userData->setValue(SERIAL_KEYMAP_EDITOR_RECT_KEY_Y, rc.top());
    m_userData->setValue(SERIAL_KEYMAP_EDITOR_RECT_KEY_W, rc.width());
    m_userData->setValue(SERIAL_KEYMAP_EDITOR_RECT_KEY_H, rc.height());
    m_userData->endGroup();
    m_userData->sync();
}

QRect Config::getKeymapEditorRect(const QString &serial)
{
    QRect rc;
    m_userData->beginGroup(serial);
    rc.setX(m_userData->value(SERIAL_KEYMAP_EDITOR_RECT_KEY_X, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setY(m_userData->value(SERIAL_KEYMAP_EDITOR_RECT_KEY_Y, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setWidth(m_userData->value(SERIAL_KEYMAP_EDITOR_RECT_KEY_W, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    rc.setHeight(m_userData->value(SERIAL_KEYMAP_EDITOR_RECT_KEY_H, SERIAL_WINDOW_RECT_KEY_DEF).toInt());
    m_userData->endGroup();
    return rc;
}

bool Config::isDeviceCenterCropEnabled(const QString &serial)
{
    return getDeviceCenterCropSize(serial) > 0;
}

int Config::getDeviceCenterCropSize(const QString &serial)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return 0;
    }

    m_userData->beginGroup(trimmedSerial);
    bool intOk = false;
    const int cropSize = m_userData->value(COMMON_VIDEO_CENTER_CROP_SIZE_KEY, 0).toInt(&intOk);
    m_userData->endGroup();
    if (!intOk || cropSize <= 0) {
        return 0;
    }

    return qBound(2, cropSize, 4096);
}

void Config::setDeviceCenterCropSize(const QString &serial, int cropSize)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return;
    }

    m_userData->beginGroup(trimmedSerial);
    m_userData->setValue(COMMON_VIDEO_CENTER_CROP_SIZE_KEY, qBound(2, cropSize, 4096));
    m_userData->endGroup();
    m_userData->sync();
}

void Config::clearDeviceCenterCropSize(const QString &serial)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return;
    }

    m_userData->beginGroup(trimmedSerial);
    m_userData->remove(COMMON_VIDEO_CENTER_CROP_SIZE_KEY);
    m_userData->endGroup();
    m_userData->sync();
}

DeviceMouseConfig Config::getDeviceMouseConfig(const QString &serial)
{
    DeviceMouseConfig config;

    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        config.cursorSizePx = qBound(8, config.cursorSizePx, 128);
        return config;
    }

    m_userData->beginGroup(trimmedSerial);
    bool intOk = false;

    QVariant value = m_userData->value(SERIAL_REMOTE_CURSOR_ENABLED_KEY);
    if (value.isValid()) {
        config.remoteCursorEnabled = parseBoolSetting(value, config.remoteCursorEnabled);
    }

    value = m_userData->value(SERIAL_CURSOR_SIZE_PX_KEY);
    if (value.isValid()) {
        const int parsedCursorSize = value.toInt(&intOk);
        if (intOk) {
            config.cursorSizePx = parsedCursorSize;
        }
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_COMPAT_ENABLED_KEY);
    if (value.isValid()) {
        config.normalMouseCompatEnabled = parseBoolSetting(value, false);
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_TOUCH_PRIORITY_ENABLED_KEY);
    if (value.isValid()) {
        config.normalMouseTouchPriorityEnabled = parseBoolSetting(value, true);
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_CURSOR_THROTTLE_ENABLED_KEY);
    if (value.isValid()) {
        config.normalMouseCursorThrottleEnabled = parseBoolSetting(value, true);
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_CURSOR_FLUSH_INTERVAL_MS_KEY);
    if (value.isValid()) {
        config.normalMouseCursorFlushIntervalMs = value.toInt(&intOk);
        if (!intOk) {
            config.normalMouseCursorFlushIntervalMs = 33;
        }
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_CURSOR_CLICK_SUPPRESSION_MS_KEY);
    if (value.isValid()) {
        config.normalMouseCursorClickSuppressionMs = value.toInt(&intOk);
        if (!intOk) {
            config.normalMouseCursorClickSuppressionMs = 120;
        }
    }

    value = m_userData->value(SERIAL_NORMAL_MOUSE_TAP_MIN_HOLD_MS_KEY);
    if (value.isValid()) {
        config.normalMouseTapMinHoldMs = value.toInt(&intOk);
        if (!intOk) {
            config.normalMouseTapMinHoldMs = 16;
        }
    }

    m_userData->endGroup();

    config.cursorSizePx = qBound(8, config.cursorSizePx, 128);
    config.normalMouseCursorFlushIntervalMs = qBound(16, config.normalMouseCursorFlushIntervalMs, 100);
    config.normalMouseCursorClickSuppressionMs = qBound(0, config.normalMouseCursorClickSuppressionMs, 300);
    config.normalMouseTapMinHoldMs = qBound(0, config.normalMouseTapMinHoldMs, 40);
    return config;
}

void Config::ensureDeviceMouseConfigInitialized(const QString &serial)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return;
    }

    const DeviceMouseConfig config = getDeviceMouseConfig(trimmedSerial);

    m_userData->beginGroup(trimmedSerial);
    const bool initialized = hasCompleteDeviceMouseConfig(m_userData);
    m_userData->endGroup();

    if (!initialized) {
        setDeviceMouseConfig(trimmedSerial, config);
    }
}

void Config::setDeviceMouseConfig(const QString &serial, const DeviceMouseConfig &config)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return;
    }

    m_userData->beginGroup(trimmedSerial);
    m_userData->setValue(SERIAL_REMOTE_CURSOR_ENABLED_KEY, config.remoteCursorEnabled);
    m_userData->setValue(SERIAL_CURSOR_SIZE_PX_KEY, qBound(8, config.cursorSizePx, 128));
    m_userData->setValue(SERIAL_NORMAL_MOUSE_COMPAT_ENABLED_KEY, config.normalMouseCompatEnabled);
    m_userData->setValue(SERIAL_NORMAL_MOUSE_TOUCH_PRIORITY_ENABLED_KEY, config.normalMouseTouchPriorityEnabled);
    m_userData->setValue(SERIAL_NORMAL_MOUSE_CURSOR_THROTTLE_ENABLED_KEY, config.normalMouseCursorThrottleEnabled);
    m_userData->setValue(SERIAL_NORMAL_MOUSE_CURSOR_FLUSH_INTERVAL_MS_KEY,
                         qBound(16, config.normalMouseCursorFlushIntervalMs, 100));
    m_userData->setValue(SERIAL_NORMAL_MOUSE_CURSOR_CLICK_SUPPRESSION_MS_KEY,
                         qBound(0, config.normalMouseCursorClickSuppressionMs, 300));
    m_userData->setValue(SERIAL_NORMAL_MOUSE_TAP_MIN_HOLD_MS_KEY,
                         qBound(0, config.normalMouseTapMinHoldMs, 40));
    m_userData->endGroup();
    m_userData->sync();
}

void Config::setNickName(const QString &serial, const QString &name)
{
    m_userData->beginGroup(serial);
    m_userData->setValue(SERIAL_NICK_NAME_KEY, name);
    m_userData->endGroup();
    m_userData->sync();
}

QString Config::getNickName(const QString &serial)
{
    QString name;
    m_userData->beginGroup(serial);
    name = m_userData->value(SERIAL_NICK_NAME_KEY, SERIAL_NICK_NAME_DEF).toString();
    m_userData->endGroup();
    return name;
}

int Config::getGlobalMaxFps()
{
    int fps = COMMON_MAX_FPS_DEF;

    m_userData->beginGroup(GROUP_COMMON);
    bool intOk = false;
    const QVariant userValue = m_userData->value(COMMON_MAX_FPS_KEY);
    if (userValue.isValid()) {
        fps = userValue.toInt(&intOk);
        if (!intOk) {
            fps = COMMON_MAX_FPS_DEF;
        }
    } else {
        m_settings->beginGroup(GROUP_COMMON);
        fps = m_settings->value(COMMON_MAX_FPS_KEY, COMMON_MAX_FPS_DEF).toInt(&intOk);
        m_settings->endGroup();
        if (!intOk) {
            fps = COMMON_MAX_FPS_DEF;
        }
    }
    m_userData->endGroup();

    return qBound(0, fps, 240);
}

int Config::getMaxFps()
{
    return getGlobalMaxFps();
}

int Config::getDesktopOpenGL()
{
    int opengl = 0;
    m_settings->beginGroup(GROUP_COMMON);
    opengl = m_settings->value(COMMON_DESKTOP_OPENGL_KEY, COMMON_DESKTOP_OPENGL_DEF).toInt();
    m_settings->endGroup();
    return opengl;
}

int Config::getSkin()
{
    // force disable skin
    return 0;
    int skin = 1;
    m_settings->beginGroup(GROUP_COMMON);
    skin = m_settings->value(COMMON_SKIN_KEY, COMMON_SKIN_DEF).toInt();
    m_settings->endGroup();
    return skin;
}

int Config::getRenderExpiredFrames()
{
    int renderExpiredFrames = 1;
    m_settings->beginGroup(GROUP_COMMON);
    renderExpiredFrames = m_settings->value(COMMON_RENDER_EXPIRED_FRAMES_KEY, COMMON_RENDER_EXPIRED_FRAMES_DEF).toInt();
    m_settings->endGroup();
    return renderExpiredFrames;
}

QString Config::getPushFilePath()
{
    QString pushFile;
    m_settings->beginGroup(GROUP_COMMON);
    pushFile = m_settings->value(COMMON_PUSHFILE_KEY, COMMON_PUSHFILE_DEF).toString();
    m_settings->endGroup();
    return pushFile;
}

QString Config::getServerPath()
{
    QString serverPath;
    m_settings->beginGroup(GROUP_COMMON);
    serverPath = m_settings->value(COMMON_SERVER_PATH_KEY, COMMON_SERVER_PATH_DEF).toString();
    m_settings->endGroup();
    return serverPath;
}

QString Config::getAdbPath()
{
    QString adbPath;
    m_settings->beginGroup(GROUP_COMMON);
    adbPath = m_settings->value(COMMON_ADB_PATH_KEY, COMMON_ADB_PATH_DEF).toString();
    m_settings->endGroup();
    return adbPath;
}

QString Config::getLogLevel()
{
    QString logLevel;
    m_settings->beginGroup(GROUP_COMMON);
    logLevel = m_settings->value(COMMON_LOG_LEVEL_KEY, COMMON_LOG_LEVEL_DEF).toString();
    m_settings->endGroup();
    return logLevel;
}

QString Config::getCodecOptions()
{
    QString codecOptions;
    m_settings->beginGroup(GROUP_COMMON);
    codecOptions = m_settings->value(COMMON_CODEC_OPTIONS_KEY, COMMON_CODEC_OPTIONS_DEF).toString();
    m_settings->endGroup();
    return codecOptions;
}

QString Config::getCodecName()
{
    QString codecName;
    m_settings->beginGroup(GROUP_COMMON);
    codecName = m_settings->value(COMMON_CODEC_NAME_KEY, COMMON_CODEC_NAME_DEF).toString();
    m_settings->endGroup();
    return codecName;
}

QStringList Config::getConnectedGroups()
{
    return m_userData->childGroups();
}

void Config::deleteGroup(const QString &serial)
{
    m_userData->remove(serial);
}

QString Config::getLanguage()
{
    QString language;
    m_settings->beginGroup(GROUP_COMMON);
    language = m_settings->value(COMMON_LANGUAGE_KEY, COMMON_LANGUAGE_DEF).toString();
    m_settings->endGroup();
    return language;
}

QString Config::getTitle()
{
    QString title;
    m_settings->beginGroup(GROUP_COMMON);
    title = m_settings->value(COMMON_TITLE_KEY, COMMON_TITLE_DEF).toString();
    m_settings->endGroup();
    return title;
}

QString Config::getStartupConsoleText()
{
    QString startupConsoleText;
    m_settings->beginGroup(GROUP_COMMON);
    startupConsoleText = m_settings->value(COMMON_STARTUP_CONSOLE_TEXT_KEY, COMMON_STARTUP_CONSOLE_TEXT_DEF).toString();
    m_settings->endGroup();
    return startupConsoleText;
}

void Config::saveIpHistory(const QString &ip)
{
    QStringList ipList = getIpHistory();
    
    // 移除已存在的相同IP（避免重复）
    ipList.removeAll(ip);
    
    // 将新IP添加到开头
    ipList.prepend(ip);
    
    // 限制历史记录数量
    while (ipList.size() > IP_HISTORY_MAX) {
        ipList.removeLast();
    }
    
    m_userData->setValue(IP_HISTORY_KEY, ipList);
    m_userData->sync();
}

QStringList Config::getIpHistory()
{
    QStringList ipList = m_userData->value(IP_HISTORY_KEY, IP_HISTORY_DEF).toStringList();
    ipList.removeAll("");
    return ipList;
}

void Config::clearIpHistory()
{
    m_userData->remove(IP_HISTORY_KEY);
    m_userData->sync();
}

void Config::savePortHistory(const QString &port)
{
    QStringList portList = getPortHistory();
    
    // 移除已存在的相同Port（避免重复）
    portList.removeAll(port);
    
    // 将新Port添加到开头
    portList.prepend(port);
    
    // 限制历史记录数量
    while (portList.size() > PORT_HISTORY_MAX) {
        portList.removeLast();
    }
    
    m_userData->setValue(PORT_HISTORY_KEY, portList);
    m_userData->sync();
}

QStringList Config::getPortHistory()
{
    QStringList portList = m_userData->value(PORT_HISTORY_KEY, PORT_HISTORY_DEF).toStringList();
    portList.removeAll("");
    return portList;
}

void Config::clearPortHistory()
{
    m_userData->remove(PORT_HISTORY_KEY);
    m_userData->sync();
}
