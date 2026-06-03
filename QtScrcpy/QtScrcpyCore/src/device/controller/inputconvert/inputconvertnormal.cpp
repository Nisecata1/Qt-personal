#include <cmath>
#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMetaType>
#include <QSettings>
#include <QTimer>

#include "inputconvertnormal.h"
#include "controller.h"

namespace {
constexpr qint32 kRemoteCursorHideCoord = -1;
constexpr int kCursorConfigDebounceMs = 120;
constexpr qint64 kNormalMouseCompatReloadCheckIntervalMs = 200;
constexpr int kRemoteCursorFlushIntervalMsDefault = 33;
constexpr int kRemoteCursorClickSuppressionMsDefault = 120;
constexpr bool kRemoteCursorImmediateDefault = true;
constexpr int kRemoteCursorMaxPendingBytesDefault = 1024;
constexpr int kNormalTapMinHoldMsDefault = 16;
constexpr int kRemoteCursorFlushIntervalMsMin = 16;
constexpr int kRemoteCursorFlushIntervalMsMax = 100;
constexpr int kRemoteCursorClickSuppressionMsMin = 0;
constexpr int kRemoteCursorClickSuppressionMsMax = 300;
constexpr int kRemoteCursorMaxPendingBytesMin = 0;
constexpr int kRemoteCursorMaxPendingBytesMax = 65536;
constexpr int kNormalTapMinHoldMsMin = 0;
constexpr int kNormalTapMinHoldMsMax = 40;
constexpr qint64 kRemoteCursorDropLogIntervalMs = 1000;
constexpr int kTouchControlPriority = Qt::HighEventPriority;
constexpr int kRemoteCursorEventPriority = Qt::LowEventPriority;
constexpr int kRemoteCursorHidePriority = Qt::NormalEventPriority;
constexpr qint32 kCursorSizeMin = 8;
constexpr qint32 kCursorSizeMax = 128;
constexpr qint32 kCursorSizeDefault = 24;
constexpr char kRemoteCursorEnabledKey[] = "RemoteCursorEnabled";
constexpr char kCursorSizePxKey[] = "CursorSizePx";
constexpr char kNormalMouseCompatEnabledKey[] = "NormalMouseCompatEnabled";
constexpr char kNormalMouseTouchPriorityEnabledKey[] = "NormalMouseTouchPriorityEnabled";
constexpr char kNormalMouseCursorThrottleEnabledKey[] = "NormalMouseCursorThrottleEnabled";
constexpr char kNormalMouseCursorFlushIntervalMsKey[] = "NormalMouseCursorFlushIntervalMs";
constexpr char kNormalMouseCursorClickSuppressionMsKey[] = "NormalMouseCursorClickSuppressionMs";
constexpr char kNormalMouseTapMinHoldMsKey[] = "NormalMouseTapMinHoldMs";
constexpr char kRemoteCursorImmediateKey[] = "RemoteCursorImmediate";
constexpr char kRemoteCursorMaxPendingBytesKey[] = "RemoteCursorMaxPendingBytes";
constexpr char kRemoteCursorSdkMouseCompatEnabledKey[] = "RemoteCursorSdkMouseCompatEnabled";

QPointF mouseLocalPos(const QMouseEvent *from)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    return from->localPos();
#else
    return from->position();
#endif
}
} // namespace

InputConvertNormal::InputConvertNormal(Controller *controller)
    : InputConvertBase(controller)
{
    m_cursorConfigPath = resolveUserDataIniPath();
    initCursorConfigWatcher();
    initRemoteCursorDispatchTimers();
    reloadNormalMouseCompatConfigIfNeeded();
}

InputConvertNormal::~InputConvertNormal()
{
    // 析构前收掉普通模式遗留的触点和定时器，避免切脚本或关服务后手机端还残留按下状态。
    resetInputState();
}

void InputConvertNormal::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 统一收口普通模式鼠标输入：默认沿用 finger 路径，Moto 兼容开关打开后改走 mouse pointer 路径。
    if (!from) {
        return;
    }

    reloadNormalMouseCompatConfigIfNeeded();
    flushPendingNormalTapRelease();

    if (useThrottledNormalCursorFeedback() && m_remoteCursorEnabled) {
        switch (from->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
            enterRemoteCursorSuppressionWindow();
            break;
        default:
            break;
        }
    }

    if (useNormalCursorFeedback() && from->type() == QEvent::MouseMove && from->buttons() == Qt::NoButton) {
        handleConfiguredCursorFeedback(from, frameSize, showSize);
    }

    // action
    AndroidMotioneventAction action;
    switch (from->type()) {
    case QEvent::MouseButtonPress:
        action = AMOTION_EVENT_ACTION_DOWN;
        beginNormalTapTracking(from);
        break;
    case QEvent::MouseButtonRelease:
        action = AMOTION_EVENT_ACTION_UP;
        if (maybeDelayNormalTapRelease(from, frameSize, showSize)) {
            return;
        }
        m_leftTapCandidate = false;
        m_leftPressStartMs = 0;
        break;
    case QEvent::MouseMove:
        // only support left button drag
        if (!(from->buttons() & Qt::LeftButton)) {
            return;
        }
        action = AMOTION_EVENT_ACTION_MOVE;
        m_leftTapCandidate = false;
        break;
    default:
        return;
    }

    QPoint touchPos;
    if (!resolveTouchPosition(from, frameSize, showSize, &touchPos)) {
        return;
    }

    hideRemoteCursorImmediately();

    const bool sent = sendTouchEvent(action,
                                     touchPos,
                                     frameSize,
                                     convertMouseButton(from->button()),
                                     convertMouseButtons(from->buttons()),
                                     resolveNormalTouchPointerId(action),
                                     false,
                                     useNormalMouseTouchPriority() ? kTouchControlPriority : Qt::NormalEventPriority);
    if (sent && action == AMOTION_EVENT_ACTION_UP && m_remoteCursorEnabled) {
        sendCursorPositionFromMouseEvent(from, frameSize, showSize);
    }
}

void InputConvertNormal::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from || from->angleDelta().isNull()) {
        return;
    }

    // delta
    float hScroll = from->angleDelta().x() / 64.0f;
    float vScroll = from->angleDelta().y() / 64.0f;

    // pos
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    QPointF pos = from->position();
#else
    QPointF pos = from->posF();
#endif
    // convert pos
    pos.setX(pos.x() * frameSize.width() / showSize.width());
    pos.setY(pos.y() * frameSize.height() / showSize.height());

    // set data
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_SCROLL);
    if (!controlMsg) {
        return;
    }
    controlMsg->setInjectScrollMsgData(QRect(pos.toPoint(), frameSize), hScroll, vScroll, convertMouseButtons(from->buttons()));
    sendControlMsg(controlMsg);
}

void InputConvertNormal::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    Q_UNUSED(frameSize)
    Q_UNUSED(showSize)
    if (!from) {
        return;
    }

    bool repeat = from->isAutoRepeat();

    // action
    AndroidKeyeventAction action;
    switch (from->type()) {
    case QEvent::KeyPress:
        action = AKEY_EVENT_ACTION_DOWN;
        break;
    case QEvent::KeyRelease:
        action = AKEY_EVENT_ACTION_UP;
        break;
    default:
        return;
    }

    // key code
    AndroidKeycode keyCode = convertKeyCode(from->key(), from->modifiers());
    if (AKEYCODE_UNKNOWN == keyCode) {
        return;
    }

    // set data
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlMsg) {
        return;
    }

    if (repeat) {
        m_repeat++;
    } else {
        m_repeat = 0;
    }

    controlMsg->setInjectKeycodeMsgData(action, keyCode, m_repeat, convertMetastate(from->modifiers()));
    sendControlMsg(controlMsg);
}

void InputConvertNormal::resetInputState()
{
    // 供 Controller 和 Device 在删转换器、停服务前调用，确保普通模式的按下触点一定补发 UP。
    flushPendingNormalTapRelease(true);
    if (m_activeNormalTouchDown && !m_activeNormalTouchFrameSize.isEmpty()) {
        sendTouchEvent(AMOTION_EVENT_ACTION_UP,
                       m_activeNormalTouchPos,
                       m_activeNormalTouchFrameSize,
                       static_cast<AndroidMotioneventButtons>(0),
                       static_cast<AndroidMotioneventButtons>(0),
                       m_activeNormalTouchPointerId,
                       true);
    }
    clearPendingRemoteCursorState();
    clearPendingNormalTapRelease();
    m_activeNormalTouchPos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_activeNormalTouchFrameSize = QSize();
    m_activeNormalTouchPointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
    m_activeNormalTouchDown = false;
    hideRemoteCursorImmediately();
}

void InputConvertNormal::handleConfiguredCursorFeedback(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 只负责普通模式下的远端黑光标反馈；Moto 兼容开启时，位置包和 hover 包从这里一起发出。
    if (!from || from->type() != QEvent::MouseMove) {
        return;
    }

    reloadNormalMouseCompatConfigIfNeeded();
    flushPendingNormalTapRelease();

    if (m_remoteCursorEnabled) {
        if (useThrottledNormalCursorFeedback() && !m_remoteCursorThrottleLogPrinted) {
            qInfo() << "remote cursor normal-mode throttling enabled:"
                    << "flushIntervalMs=" << m_normalMouseCompatConfig.cursorFlushIntervalMs
                    << "clickSuppressionMs=" << m_normalMouseCompatConfig.clickSuppressionMs;
            m_remoteCursorThrottleLogPrinted = true;
        }
        if (m_lastSentCursorSizePx != m_cursorSizePx) {
            sendCursorConfigEvent(m_cursorSizePx);
        }
        if (useThrottledNormalCursorFeedback()) {
            queueCursorPositionFromMouseEvent(from, frameSize, showSize);
        } else {
            sendCursorPositionFromMouseEvent(from, frameSize, showSize);
        }
    } else {
        sendCursorHideEvent();
    }
}

bool InputConvertNormal::useNormalCursorFeedback() const
{
    return true;
}

bool InputConvertNormal::useThrottledNormalCursorFeedback() const
{
    return m_normalMouseCompatConfig.enabled && m_normalMouseCompatConfig.cursorThrottleEnabled;
}

bool InputConvertNormal::useNormalMouseTouchPriority() const
{
    // 读取普通模式点击是否提到高优先级队列；Moto 兼容分支继续复用这套优先级开关。
    return m_normalMouseCompatConfig.enabled && m_normalMouseCompatConfig.touchPriorityEnabled;
}

bool InputConvertNormal::useRemoteCursorSdkMouseCompat() const
{
    // 隐藏的 Moto 兼容分支只在普通鼠标兼容总开关和远端光标都打开时才生效，避免默认普通点击额外带上 hover 包。
    return m_remoteCursorEnabled
        && m_normalMouseCompatConfig.enabled
        && m_normalMouseCompatConfig.remoteCursorSdkMouseCompatEnabled;
}

quint64 InputConvertNormal::resolveNormalTouchPointerId(AndroidMotioneventAction action) const
{
    // 普通模式按下时用当前配置选 pointerId，移动和抬起优先跟随已按下的 pointerId，避免中途切配置导致 UP 对不上。
    if ((action == AMOTION_EVENT_ACTION_MOVE || action == AMOTION_EVENT_ACTION_UP) && m_activeNormalTouchDown) {
        return m_activeNormalTouchPointerId;
    }
    return useRemoteCursorSdkMouseCompat()
        ? static_cast<quint64>(POINTER_ID_MOUSE)
        : static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
}

QString InputConvertNormal::resolveUserDataIniPath() const
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

void InputConvertNormal::reloadNormalMouseCompatConfigIfNeeded()
{
    // 读取普通鼠标和远端光标的热更新配置，优先保留设备分组覆盖公共默认值。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_normalMouseCompatConfig.initialized
        && nowMs - m_normalMouseCompatConfig.lastCheckMs < kNormalMouseCompatReloadCheckIntervalMs) {
        return;
    }
    m_normalMouseCompatConfig.lastCheckMs = nowMs;

    if (m_normalMouseCompatConfig.iniPath.isEmpty()) {
        m_normalMouseCompatConfig.iniPath = resolveUserDataIniPath();
    }

    QFileInfo fileInfo(m_normalMouseCompatConfig.iniPath);
    const qint64 modifiedMs = fileInfo.exists() ? fileInfo.lastModified().toMSecsSinceEpoch() : -1;
    if (m_normalMouseCompatConfig.initialized
        && modifiedMs == m_normalMouseCompatConfig.lastModifiedMs) {
        return;
    }

    bool remoteCursorEnabled = false;
    int cursorSizePx = kCursorSizeDefault;
    bool enabled = false;
    bool touchPriorityEnabled = true;
    bool cursorThrottleEnabled = true;
    int cursorFlushIntervalMs = kRemoteCursorFlushIntervalMsDefault;
    int clickSuppressionMs = kRemoteCursorClickSuppressionMsDefault;
    int tapMinHoldMs = kNormalTapMinHoldMsDefault;
    bool remoteCursorImmediate = kRemoteCursorImmediateDefault;
    int remoteCursorMaxPendingBytes = kRemoteCursorMaxPendingBytesDefault;
    bool remoteCursorSdkMouseCompatEnabled = false;

    const QString serial = m_controller ? m_controller->getDeviceSerial().trimmed() : QString();
    if (fileInfo.exists() && fileInfo.isFile()) {
        QSettings settings(m_normalMouseCompatConfig.iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        settings.setIniCodec("UTF-8");
#endif

        bool ok = false;
        bool intOk = false;

        const QVariant commonCursorImmediateValue = settings.value(QStringLiteral("common/") + kRemoteCursorImmediateKey);
        if (commonCursorImmediateValue.isValid()) {
            remoteCursorImmediate = parseBoolSetting(commonCursorImmediateValue, remoteCursorImmediate, &ok);
        }

        const QVariant commonCursorMaxPendingValue = settings.value(QStringLiteral("common/") + kRemoteCursorMaxPendingBytesKey);
        if (commonCursorMaxPendingValue.isValid()) {
            const int parsedMaxPendingBytes = commonCursorMaxPendingValue.toInt(&intOk);
            if (intOk) {
                remoteCursorMaxPendingBytes = parsedMaxPendingBytes;
            }
        }

        if (serial.isEmpty()) {
            remoteCursorMaxPendingBytes = qBound(kRemoteCursorMaxPendingBytesMin,
                                                 remoteCursorMaxPendingBytes,
                                                 kRemoteCursorMaxPendingBytesMax);
        } else {
            const QString groupPrefix = serial + "/";
            const QVariant remoteCursorEnabledValue = settings.value(groupPrefix + kRemoteCursorEnabledKey);
            if (remoteCursorEnabledValue.isValid()) {
                remoteCursorEnabled = parseBoolSetting(remoteCursorEnabledValue, remoteCursorEnabled, &ok);
            }

            const QVariant cursorSizeValue = settings.value(groupPrefix + kCursorSizePxKey);
            if (cursorSizeValue.isValid()) {
                const int parsedCursorSize = cursorSizeValue.toInt(&intOk);
                if (intOk) {
                    cursorSizePx = parsedCursorSize;
                }
            }

            enabled = parseBoolSetting(settings.value(groupPrefix + kNormalMouseCompatEnabledKey), false, &ok);
            if (!ok) {
                enabled = false;
            }

            if (enabled) {
                touchPriorityEnabled = parseBoolSetting(settings.value(groupPrefix + kNormalMouseTouchPriorityEnabledKey),
                                                       true, &ok);
                if (!ok) {
                    touchPriorityEnabled = true;
                }

                cursorThrottleEnabled = parseBoolSetting(settings.value(groupPrefix + kNormalMouseCursorThrottleEnabledKey),
                                                         true, &ok);
                if (!ok) {
                    cursorThrottleEnabled = true;
                }

                cursorFlushIntervalMs = settings.value(groupPrefix + kNormalMouseCursorFlushIntervalMsKey,
                                                       kRemoteCursorFlushIntervalMsDefault).toInt(&intOk);
                if (!intOk) {
                    cursorFlushIntervalMs = kRemoteCursorFlushIntervalMsDefault;
                }

                clickSuppressionMs = settings.value(groupPrefix + kNormalMouseCursorClickSuppressionMsKey,
                                                    kRemoteCursorClickSuppressionMsDefault).toInt(&intOk);
                if (!intOk) {
                    clickSuppressionMs = kRemoteCursorClickSuppressionMsDefault;
                }

                tapMinHoldMs = settings.value(groupPrefix + kNormalMouseTapMinHoldMsKey,
                                              kNormalTapMinHoldMsDefault).toInt(&intOk);
                if (!intOk) {
                    tapMinHoldMs = kNormalTapMinHoldMsDefault;
                }
            }

            const QVariant cursorImmediateValue = settings.value(groupPrefix + kRemoteCursorImmediateKey);
            if (cursorImmediateValue.isValid()) {
                remoteCursorImmediate = parseBoolSetting(cursorImmediateValue, remoteCursorImmediate, &ok);
            }

            const QVariant cursorMaxPendingValue = settings.value(groupPrefix + kRemoteCursorMaxPendingBytesKey);
            if (cursorMaxPendingValue.isValid()) {
                const int parsedMaxPendingBytes = cursorMaxPendingValue.toInt(&intOk);
                if (intOk) {
                    remoteCursorMaxPendingBytes = parsedMaxPendingBytes;
                }
            }

            const QVariant remoteCursorSdkMouseCompatValue = settings.value(groupPrefix + kRemoteCursorSdkMouseCompatEnabledKey);
            if (remoteCursorSdkMouseCompatValue.isValid()) {
                remoteCursorSdkMouseCompatEnabled = parseBoolSetting(remoteCursorSdkMouseCompatValue,
                                                                    remoteCursorSdkMouseCompatEnabled,
                                                                    &ok);
            }
        }
    }

    cursorSizePx = qBound(static_cast<int>(kCursorSizeMin), cursorSizePx, static_cast<int>(kCursorSizeMax));
    cursorFlushIntervalMs = qBound(kRemoteCursorFlushIntervalMsMin, cursorFlushIntervalMs, kRemoteCursorFlushIntervalMsMax);
    clickSuppressionMs = qBound(kRemoteCursorClickSuppressionMsMin, clickSuppressionMs, kRemoteCursorClickSuppressionMsMax);
    tapMinHoldMs = qBound(kNormalTapMinHoldMsMin, tapMinHoldMs, kNormalTapMinHoldMsMax);
    remoteCursorMaxPendingBytes = qBound(kRemoteCursorMaxPendingBytesMin,
                                         remoteCursorMaxPendingBytes,
                                         kRemoteCursorMaxPendingBytesMax);

    const bool changed = !m_normalMouseCompatConfig.initialized
        || m_remoteCursorEnabled != remoteCursorEnabled
        || m_cursorSizePx != cursorSizePx
        || m_normalMouseCompatConfig.enabled != enabled
        || m_normalMouseCompatConfig.touchPriorityEnabled != touchPriorityEnabled
        || m_normalMouseCompatConfig.cursorThrottleEnabled != cursorThrottleEnabled
        || m_normalMouseCompatConfig.cursorFlushIntervalMs != cursorFlushIntervalMs
        || m_normalMouseCompatConfig.clickSuppressionMs != clickSuppressionMs
        || m_normalMouseCompatConfig.tapMinHoldMs != tapMinHoldMs
        || m_normalMouseCompatConfig.remoteCursorImmediate != remoteCursorImmediate
        || m_normalMouseCompatConfig.remoteCursorMaxPendingBytes != remoteCursorMaxPendingBytes
        || m_normalMouseCompatConfig.remoteCursorSdkMouseCompatEnabled != remoteCursorSdkMouseCompatEnabled;

    const bool cursorSizeChanged = (m_cursorSizePx != cursorSizePx);
    m_remoteCursorEnabled = remoteCursorEnabled;
    m_cursorSizePx = cursorSizePx;
    if (cursorSizeChanged) {
        m_lastSentCursorSizePx = -1;
    }
    m_normalMouseCompatConfig.enabled = enabled;
    m_normalMouseCompatConfig.touchPriorityEnabled = touchPriorityEnabled;
    m_normalMouseCompatConfig.cursorThrottleEnabled = cursorThrottleEnabled;
    m_normalMouseCompatConfig.cursorFlushIntervalMs = cursorFlushIntervalMs;
    m_normalMouseCompatConfig.clickSuppressionMs = clickSuppressionMs;
    m_normalMouseCompatConfig.tapMinHoldMs = tapMinHoldMs;
    m_normalMouseCompatConfig.remoteCursorImmediate = remoteCursorImmediate;
    m_normalMouseCompatConfig.remoteCursorMaxPendingBytes = remoteCursorMaxPendingBytes;
    m_normalMouseCompatConfig.remoteCursorSdkMouseCompatEnabled = remoteCursorSdkMouseCompatEnabled;
    m_normalMouseCompatConfig.lastModifiedMs = modifiedMs;
    m_normalMouseCompatConfig.initialized = true;

    if (changed) {
        applyNormalMouseCompatTimerConfig();
        m_remoteCursorThrottleLogPrinted = false;
        clearPendingRemoteCursorState();
        if (!m_remoteCursorEnabled) {
            sendCursorHideEvent();
        } else if (m_remoteCursorVisible && m_lastSentCursorSizePx != m_cursorSizePx) {
            sendCursorConfigEvent(m_cursorSizePx);
        }
    }
}

bool InputConvertNormal::sendCursorPositionEvent(qint32 x, qint32 y, qint32 w, qint32 h)
{
    // 使用当前普通鼠标优先级设置发送远端光标位置，并把发送结果返回给调用方更新本地状态。
    return sendCursorPositionEvent(x, y, w, h, useNormalMouseTouchPriority() ? kRemoteCursorEventPriority : Qt::NormalEventPriority);
}

bool InputConvertNormal::sendCursorPositionEvent(qint32 x, qint32 y, qint32 w, qint32 h, int priority)
{
    // 远端光标位置只保留最新值；控制 socket 积压时丢弃本次位置，避免手机端慢放旧坐标。
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_CURSOR);
    if (!controlMsg) {
        return false;
    }

    const bool hideCursor = (x == kRemoteCursorHideCoord && y == kRemoteCursorHideCoord && w == 0 && h == 0);
    const int maxPendingBytes = m_normalMouseCompatConfig.remoteCursorMaxPendingBytes;
    if (!hideCursor && maxPendingBytes > 0) {
        const qint64 pendingBytes = pendingControlBytes();
        if (pendingBytes >= maxPendingBytes) {
            delete controlMsg;
            ++m_remoteCursorDroppedSinceLastLog;

            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_remoteCursorDropLastLogMs <= 0 || nowMs - m_remoteCursorDropLastLogMs >= kRemoteCursorDropLogIntervalMs) {
                qInfo() << "remote cursor position dropped:"
                        << "count=" << m_remoteCursorDroppedSinceLastLog
                        << "pendingBytes=" << pendingBytes
                        << "threshold=" << maxPendingBytes;
                m_remoteCursorDroppedSinceLastLog = 0;
                m_remoteCursorDropLastLogMs = nowMs;
            }
            return false;
        }
    }

    controlMsg->setInjectCursorMsgData(x, y, w, h);
    if (m_normalMouseCompatConfig.remoteCursorImmediate) {
        return sendControlMsgImmediately(controlMsg);
    }
    if (!m_controller) {
        delete controlMsg;
        return false;
    }
    sendControlMsg(controlMsg, priority);
    return true;
}

void InputConvertNormal::sendCursorConfigEvent(qint32 cursorSizePx)
{
    sendCursorConfigEvent(cursorSizePx, useNormalMouseTouchPriority() ? kRemoteCursorEventPriority : Qt::NormalEventPriority);
}

void InputConvertNormal::sendCursorConfigEvent(qint32 cursorSizePx, int priority)
{
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_SET_CURSOR_CONFIG);
    if (!controlMsg) {
        return;
    }

    controlMsg->setCursorConfigMsgData(cursorSizePx);
    sendControlMsg(controlMsg, priority);
    m_lastSentCursorSizePx = cursorSizePx;
}

void InputConvertNormal::sendCursorHideEvent()
{
    // 普通模式离开远端光标显示时只发 hide 包，不附带 hover，避免手机端保留旧鼠标位置。
    clearPendingRemoteCursorState();

    if (!m_remoteCursorVisible) {
        return;
    }

    sendCursorPositionEvent(kRemoteCursorHideCoord, kRemoteCursorHideCoord, 0, 0, kRemoteCursorHidePriority);
    m_remoteCursorVisible = false;
    m_lastRemoteCursorPos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_lastRemoteCursorFrameSize = QSize();
}

bool InputConvertNormal::resolveTouchPosition(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize,
                                              QPoint *touchPos) const
{
    if (!from || frameSize.isEmpty() || showSize.isEmpty() || !touchPos) {
        return false;
    }

    QPointF pos = mouseLocalPos(from);
    qint32 x = qRound(pos.x() * static_cast<qreal>(frameSize.width()) / showSize.width());
    qint32 y = qRound(pos.y() * static_cast<qreal>(frameSize.height()) / showSize.height());
    x = qBound(0, x, frameSize.width() - 1);
    y = qBound(0, y, frameSize.height() - 1);
    *touchPos = QPoint(x, y);
    return true;
}

bool InputConvertNormal::sendTouchEvent(AndroidMotioneventAction action, const QPoint &touchPos, const QSize &frameSize,
                                        AndroidMotioneventButtons actionButtons, AndroidMotioneventButtons buttons, quint64 pointerId,
                                        bool immediate, int priority, bool trackActiveTouch)
{
    // 统一构造普通模式触控包；Moto 兼容开启时这里既能发 mouse pointer，也能按调用方要求跳过按下状态记录。
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        return false;
    }

    controlMsg->setInjectTouchMsgData(
        pointerId,
        action,
        actionButtons,
        buttons,
        QRect(touchPos, frameSize),
        AMOTION_EVENT_ACTION_UP == action ? 0.0f : 1.0f);
    if (trackActiveTouch) {
        if (AMOTION_EVENT_ACTION_UP == action) {
            m_activeNormalTouchDown = false;
            m_activeNormalTouchPos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
            m_activeNormalTouchFrameSize = QSize();
            m_activeNormalTouchPointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
        } else {
            m_activeNormalTouchDown = true;
            m_activeNormalTouchPos = touchPos;
            m_activeNormalTouchFrameSize = frameSize;
            m_activeNormalTouchPointerId = pointerId;
        }
    }

    if (immediate) {
        return sendControlMsgImmediately(controlMsg);
    }
    if (!m_controller) {
        delete controlMsg;
        return false;
    }
    sendControlMsg(controlMsg, priority);
    return true;
}

bool InputConvertNormal::hideRemoteCursorImmediately()
{
    // 普通模式准备发送点击或拖动前先立刻隐藏手机端黑光标，避免 Moto 一类系统把这层光标当成遮挡层后拦掉点击。
    clearPendingRemoteCursorState();

    if (!m_remoteCursorVisible) {
        return true;
    }

    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_CURSOR);
    if (!controlMsg) {
        return false;
    }

    controlMsg->setInjectCursorMsgData(kRemoteCursorHideCoord, kRemoteCursorHideCoord, 0, 0);
    if (!sendControlMsgImmediately(controlMsg)) {
        return false;
    }

    m_remoteCursorVisible = false;
    m_lastRemoteCursorPos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_lastRemoteCursorFrameSize = QSize();
    return true;
}

void InputConvertNormal::applyNormalMouseCompatTimerConfig()
{
    // 普通模式远端光标限频和点击抑制定时器都从 ini 热更新配置取值，这里集中刷新 interval。
    if (m_remoteCursorFlushTimer) {
        m_remoteCursorFlushTimer->setInterval(m_normalMouseCompatConfig.cursorFlushIntervalMs);
    }
    if (m_remoteCursorSuppressionTimer) {
        m_remoteCursorSuppressionTimer->setInterval(m_normalMouseCompatConfig.clickSuppressionMs);
    }
}

void InputConvertNormal::beginNormalTapTracking(const QMouseEvent *from)
{
    // 只给旧的 finger 点击路径记录“最短按住时间”；Moto 兼容分支改走 mouse pointer 后不再延后 UP。
    if (!from || from->button() != Qt::LeftButton || !m_normalMouseCompatConfig.enabled
        || m_normalMouseCompatConfig.tapMinHoldMs <= 0 || useRemoteCursorSdkMouseCompat()) {
        m_leftTapCandidate = false;
        m_leftPressStartMs = 0;
        return;
    }

    clearPendingNormalTapRelease();
    m_leftPressStartMs = QDateTime::currentMSecsSinceEpoch();
    m_leftTapCandidate = true;
}

bool InputConvertNormal::maybeDelayNormalTapRelease(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 只给旧的 finger 轻点补最短按住时长；Moto 兼容分支下直接按真实鼠标时序抬起。
    if (!from || from->button() != Qt::LeftButton || !m_normalMouseCompatConfig.enabled
        || m_normalMouseCompatConfig.tapMinHoldMs <= 0 || !m_leftTapCandidate || m_leftPressStartMs <= 0
        || useRemoteCursorSdkMouseCompat()) {
        return false;
    }

    QPoint touchPos;
    if (!resolveTouchPosition(from, frameSize, showSize, &touchPos)) {
        return false;
    }

    const qint64 elapsedMs = QDateTime::currentMSecsSinceEpoch() - m_leftPressStartMs;
    const int remainingMs = m_normalMouseCompatConfig.tapMinHoldMs - static_cast<int>(elapsedMs);
    if (remainingMs <= 0) {
        return false;
    }

    if (!m_normalTapReleaseTimer) {
        m_normalTapReleaseTimer = new QTimer(this);
        m_normalTapReleaseTimer->setSingleShot(true);
        connect(m_normalTapReleaseTimer, &QTimer::timeout,
                this, &InputConvertNormal::onNormalTapReleaseTimeout);
    }

    m_pendingNormalTapReleasePos = touchPos;
    m_pendingNormalTapReleaseFrameSize = frameSize;
    m_pendingNormalTapReleaseActionButtons = convertMouseButton(from->button());
    m_pendingNormalTapReleaseButtons = convertMouseButtons(from->buttons());
    m_pendingNormalTapReleasePointerId = resolveNormalTouchPointerId(AMOTION_EVENT_ACTION_UP);
    m_pendingNormalTapReleaseValid = true;
    m_leftTapCandidate = false;
    m_leftPressStartMs = 0;
    m_normalTapReleaseTimer->start(remainingMs);
    return true;
}

void InputConvertNormal::clearPendingNormalTapRelease()
{
    // 清掉延迟抬起定时器和缓存，但不主动补发 UP；真正需要兜底补发时由 resetInputState() 负责。
    if (m_normalTapReleaseTimer) {
        m_normalTapReleaseTimer->stop();
    }
    m_pendingNormalTapReleaseValid = false;
    m_pendingNormalTapReleasePos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_pendingNormalTapReleaseFrameSize = QSize();
    m_pendingNormalTapReleaseActionButtons = static_cast<AndroidMotioneventButtons>(0);
    m_pendingNormalTapReleaseButtons = static_cast<AndroidMotioneventButtons>(0);
    m_pendingNormalTapReleasePointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
    m_leftTapCandidate = false;
    m_leftPressStartMs = 0;
}

void InputConvertNormal::flushPendingNormalTapRelease(bool immediate)
{
    // 把延迟中的普通点击抬起立即发出去，避免轻点还没等到定时器触发就切脚本或断开连接。
    if (!m_pendingNormalTapReleaseValid) {
        return;
    }

    if (m_normalTapReleaseTimer) {
        m_normalTapReleaseTimer->stop();
    }

    sendTouchEvent(AMOTION_EVENT_ACTION_UP,
                   m_pendingNormalTapReleasePos,
                   m_pendingNormalTapReleaseFrameSize,
                   m_pendingNormalTapReleaseActionButtons,
                   m_pendingNormalTapReleaseButtons,
                   m_pendingNormalTapReleasePointerId,
                   immediate,
                   useNormalMouseTouchPriority() ? kTouchControlPriority : Qt::NormalEventPriority);
    m_pendingNormalTapReleaseValid = false;
    m_pendingNormalTapReleasePos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_pendingNormalTapReleaseFrameSize = QSize();
    m_pendingNormalTapReleaseActionButtons = static_cast<AndroidMotioneventButtons>(0);
    m_pendingNormalTapReleaseButtons = static_cast<AndroidMotioneventButtons>(0);
    m_pendingNormalTapReleasePointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
}

void InputConvertNormal::sendCursorPositionFromMouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 不限频时直接把这次鼠标位置送到手机；Moto 兼容开启时，同一位置还会补一条 hover 包。
    QPoint remotePos;
    QSize remoteFrameSize;
    if (!resolveRemoteCursorPosition(from, frameSize, showSize, &remotePos, &remoteFrameSize)) {
        return;
    }

    sendRemoteCursorUpdate(remotePos, remoteFrameSize, useRemoteCursorSdkMouseCompat() && from->buttons() == Qt::NoButton);
}

bool InputConvertNormal::resolveRemoteCursorPosition(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize,
                                                     QPoint *remotePos, QSize *remoteFrameSize) const
{
    if (!from || frameSize.isEmpty() || showSize.isEmpty() || !remotePos || !remoteFrameSize) {
        return false;
    }

    QPointF localPos = mouseLocalPos(from);
    qint32 x = qRound(localPos.x() * static_cast<qreal>(frameSize.width()) / showSize.width());
    qint32 y = qRound(localPos.y() * static_cast<qreal>(frameSize.height()) / showSize.height());
    x = qBound(0, x, frameSize.width() - 1);
    y = qBound(0, y, frameSize.height() - 1);

    *remotePos = QPoint(x, y);
    *remoteFrameSize = frameSize;
    return true;
}

void InputConvertNormal::queueCursorPositionFromMouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 限频模式下只保留最后一个远端光标位置；Moto 兼容开启时，把“这次是否需要 hover”也一起缓存。
    QPoint remotePos;
    QSize remoteFrameSize;
    if (!resolveRemoteCursorPosition(from, frameSize, showSize, &remotePos, &remoteFrameSize)) {
        return;
    }

    m_pendingRemoteCursorPos = remotePos;
    m_pendingRemoteCursorFrameSize = remoteFrameSize;
    m_pendingRemoteCursorValid = true;
    m_pendingRemoteCursorSendMouseHover = useRemoteCursorSdkMouseCompat() && from->buttons() == Qt::NoButton;

    if (!m_remoteCursorSuppressed && m_remoteCursorFlushTimer && !m_remoteCursorFlushTimer->isActive()) {
        m_remoteCursorFlushTimer->start();
    }
}

void InputConvertNormal::flushPendingRemoteCursor()
{
    // 按限频定时器发送最新的远端光标位置；socket 积压时直接丢弃这个位置。
    if (m_remoteCursorSuppressed || !m_pendingRemoteCursorValid) {
        return;
    }

    const QPoint remotePos = m_pendingRemoteCursorPos;
    const QSize remoteFrameSize = m_pendingRemoteCursorFrameSize;
    const bool sendMouseHover = m_pendingRemoteCursorSendMouseHover;
    m_pendingRemoteCursorValid = false;
    m_pendingRemoteCursorSendMouseHover = false;

    sendRemoteCursorUpdate(remotePos, remoteFrameSize, sendMouseHover);
}

bool InputConvertNormal::sendRemoteCursorUpdate(const QPoint &remotePos, const QSize &remoteFrameSize, bool sendMouseHover)
{
    // 把远端黑光标位置和可选的 mouse hover 一起发出去，确保 Moto 兼容分支里“看到的光标”和“系统里的鼠标位置”一致。
    if (m_remoteCursorVisible
        && remotePos == m_lastRemoteCursorPos
        && remoteFrameSize == m_lastRemoteCursorFrameSize) {
        return true;
    }

    const int priority = useNormalMouseTouchPriority() ? kRemoteCursorEventPriority : Qt::NormalEventPriority;
    if (!sendCursorPositionEvent(remotePos.x(), remotePos.y(), remoteFrameSize.width(), remoteFrameSize.height(), priority)) {
        return false;
    }

    m_remoteCursorVisible = true;
    m_lastRemoteCursorPos = remotePos;
    m_lastRemoteCursorFrameSize = remoteFrameSize;

    if (sendMouseHover) {
        sendTouchEvent(AMOTION_EVENT_ACTION_HOVER_MOVE,
                       remotePos,
                       remoteFrameSize,
                       static_cast<AndroidMotioneventButtons>(0),
                       static_cast<AndroidMotioneventButtons>(0),
                       static_cast<quint64>(POINTER_ID_MOUSE),
                       m_normalMouseCompatConfig.remoteCursorImmediate,
                       priority,
                       false);
    }
    return true;
}

bool InputConvertNormal::parseBoolSetting(const QVariant &value, bool defaultValue, bool *ok) const
{
    if (ok) {
        *ok = false;
    }

    if (!value.isValid() || value.isNull()) {
        return defaultValue;
    }

    if (value.userType() == QMetaType::Bool) {
        if (ok) {
            *ok = true;
        }
        return value.toBool();
    }

    bool intOk = false;
    int intValue = value.toInt(&intOk);
    if (intOk) {
        if (ok) {
            *ok = true;
        }
        return intValue != 0;
    }

    QString text = value.toString().trimmed().toLower();
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

void InputConvertNormal::ensureCursorConfigWatchPath()
{
    if (!m_cursorConfigWatcher) {
        return;
    }

    if (!QFileInfo::exists(m_cursorConfigPath)) {
        return;
    }

    if (!m_cursorConfigWatcher->files().contains(m_cursorConfigPath)) {
        m_cursorConfigWatcher->addPath(m_cursorConfigPath);
    }
}

void InputConvertNormal::initCursorConfigWatcher()
{
    if (m_cursorConfigWatcher) {
        return;
    }

    m_cursorConfigWatcher = new QFileSystemWatcher(this);
    connect(m_cursorConfigWatcher, &QFileSystemWatcher::fileChanged,
            this, &InputConvertNormal::onCursorConfigFileChanged);

    m_cursorConfigDebounceTimer = new QTimer(this);
    m_cursorConfigDebounceTimer->setSingleShot(true);
    connect(m_cursorConfigDebounceTimer, &QTimer::timeout,
            this, &InputConvertNormal::onCursorConfigDebounced);

    ensureCursorConfigWatchPath();
}

void InputConvertNormal::initRemoteCursorDispatchTimers()
{
    if (!m_remoteCursorFlushTimer) {
        m_remoteCursorFlushTimer = new QTimer(this);
        connect(m_remoteCursorFlushTimer, &QTimer::timeout,
                this, &InputConvertNormal::onRemoteCursorFlushTimer);
    }

    if (!m_remoteCursorSuppressionTimer) {
        m_remoteCursorSuppressionTimer = new QTimer(this);
        m_remoteCursorSuppressionTimer->setSingleShot(true);
        connect(m_remoteCursorSuppressionTimer, &QTimer::timeout,
                this, &InputConvertNormal::onRemoteCursorSuppressionTimeout);
    }

    applyNormalMouseCompatTimerConfig();
}

void InputConvertNormal::enterRemoteCursorSuppressionWindow()
{
    if (!useThrottledNormalCursorFeedback()) {
        return;
    }

    if (m_remoteCursorFlushTimer) {
        m_remoteCursorFlushTimer->stop();
    }

    const bool wasSuppressed = m_remoteCursorSuppressed;
    m_remoteCursorSuppressed = true;
    if (m_remoteCursorSuppressionTimer) {
        m_remoteCursorSuppressionTimer->start();
    }
    if (!wasSuppressed) {
        qDebug() << "suppressing remote cursor updates during click burst";
    }
}

void InputConvertNormal::clearPendingRemoteCursorState()
{
    // 这里只清远端黑光标相关的节流状态，避免误把待发送的点击抬起一起取消掉。
    if (m_remoteCursorFlushTimer) {
        m_remoteCursorFlushTimer->stop();
    }
    if (m_remoteCursorSuppressionTimer) {
        m_remoteCursorSuppressionTimer->stop();
    }

    m_remoteCursorSuppressed = false;
    m_pendingRemoteCursorValid = false;
    m_pendingRemoteCursorPos = QPoint(kRemoteCursorHideCoord, kRemoteCursorHideCoord);
    m_pendingRemoteCursorFrameSize = QSize();
    m_pendingRemoteCursorSendMouseHover = false;
}

void InputConvertNormal::onCursorConfigFileChanged(const QString &path)
{
    Q_UNUSED(path)
    if (!m_cursorConfigDebounceTimer) {
        return;
    }

    m_cursorConfigDebounceTimer->start(kCursorConfigDebounceMs);
}

void InputConvertNormal::onCursorConfigDebounced()
{
    m_normalMouseCompatConfig.lastCheckMs = 0;
    m_normalMouseCompatConfig.lastModifiedMs = -2;
    reloadNormalMouseCompatConfigIfNeeded();
    ensureCursorConfigWatchPath();
}

void InputConvertNormal::onRemoteCursorFlushTimer()
{
    if (m_remoteCursorSuppressed) {
        if (m_remoteCursorFlushTimer) {
            m_remoteCursorFlushTimer->stop();
        }
        return;
    }

    flushPendingRemoteCursor();
    if (m_remoteCursorFlushTimer && !m_pendingRemoteCursorValid) {
        m_remoteCursorFlushTimer->stop();
    }
}

void InputConvertNormal::onRemoteCursorSuppressionTimeout()
{
    m_remoteCursorSuppressed = false;
    flushPendingRemoteCursor();
    if (m_remoteCursorFlushTimer && m_pendingRemoteCursorValid && !m_remoteCursorFlushTimer->isActive()) {
        m_remoteCursorFlushTimer->start();
    }
}

void InputConvertNormal::onNormalTapReleaseTimeout()
{
    flushPendingNormalTapRelease();
}

AndroidMotioneventButtons InputConvertNormal::convertMouseButtons(Qt::MouseButtons buttonState)
{
    quint32 buttons = 0;
    if (buttonState & Qt::LeftButton) {
        buttons |= AMOTION_EVENT_BUTTON_PRIMARY;
    }
    if (buttonState & Qt::RightButton) {
        buttons |= AMOTION_EVENT_BUTTON_SECONDARY;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    if (buttonState & Qt::MiddleButton) {
#else
    if (buttonState & Qt::MidButton) {
#endif    
        buttons |= AMOTION_EVENT_BUTTON_TERTIARY;
    }
    if (buttonState & Qt::XButton1) {
        buttons |= AMOTION_EVENT_BUTTON_BACK;
    }
    if (buttonState & Qt::XButton2) {
        buttons |= AMOTION_EVENT_BUTTON_FORWARD;
    }
    return static_cast<AndroidMotioneventButtons>(buttons);
}

AndroidMotioneventButtons InputConvertNormal::convertMouseButton(Qt::MouseButton button)
{
    if (button == Qt::LeftButton) {
        return AMOTION_EVENT_BUTTON_PRIMARY;
    }
    if (button == Qt::RightButton) {
        return AMOTION_EVENT_BUTTON_SECONDARY;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(5, 15, 0))
    if (button == Qt::MiddleButton) {
#else
    if (button == Qt::MidButton) {
#endif
        return AMOTION_EVENT_BUTTON_TERTIARY;
    }
    if (button == Qt::XButton1) {
        return AMOTION_EVENT_BUTTON_BACK;
    }
    if (button == Qt::XButton2) {
        return AMOTION_EVENT_BUTTON_FORWARD;
    }

    return static_cast<AndroidMotioneventButtons>(0);
}

AndroidKeycode InputConvertNormal::convertKeyCode(int key, Qt::KeyboardModifiers modifiers)
{
    AndroidKeycode keyCode = AKEYCODE_UNKNOWN;
    // functional keys
    switch (key) {
    case Qt::Key_Return:
        keyCode = AKEYCODE_ENTER;
        break;
    case Qt::Key_Enter:
        keyCode = AKEYCODE_NUMPAD_ENTER;
        break;
    case Qt::Key_Escape:
        keyCode = AKEYCODE_ESCAPE;
        break;
    case Qt::Key_Backspace:
        keyCode = AKEYCODE_DEL;
        break;
    case Qt::Key_Delete:
        keyCode = AKEYCODE_FORWARD_DEL;
        break;
    case Qt::Key_Tab:
        keyCode = AKEYCODE_TAB;
        break;
    case Qt::Key_Home:
        keyCode = AKEYCODE_MOVE_HOME;
        break;
    case Qt::Key_End:
        keyCode = AKEYCODE_MOVE_END;
        break;
    case Qt::Key_PageUp:
        keyCode = AKEYCODE_PAGE_UP;
        break;
    case Qt::Key_PageDown:
        keyCode = AKEYCODE_PAGE_DOWN;
        break;
    case Qt::Key_Left:
        keyCode = AKEYCODE_DPAD_LEFT;
        break;
    case Qt::Key_Right:
        keyCode = AKEYCODE_DPAD_RIGHT;
        break;
    case Qt::Key_Up:
        keyCode = AKEYCODE_DPAD_UP;
        break;
    case Qt::Key_Down:
        keyCode = AKEYCODE_DPAD_DOWN;
        break;
    }
    if (AKEYCODE_UNKNOWN != keyCode) {
        return keyCode;
    }

    // if ALT and META are pressed, dont handle letters and space
    if (modifiers & (Qt::AltModifier | Qt::MetaModifier)) {
        return keyCode;
    }

    // character keys
    switch (key) {
    case Qt::Key_A:
        keyCode = AKEYCODE_A;
        break;
    case Qt::Key_B:
        keyCode = AKEYCODE_B;
        break;
    case Qt::Key_C:
        keyCode = AKEYCODE_C;
        break;
    case Qt::Key_D:
        keyCode = AKEYCODE_D;
        break;
    case Qt::Key_E:
        keyCode = AKEYCODE_E;
        break;
    case Qt::Key_F:
        keyCode = AKEYCODE_F;
        break;
    case Qt::Key_G:
        keyCode = AKEYCODE_G;
        break;
    case Qt::Key_H:
        keyCode = AKEYCODE_H;
        break;
    case Qt::Key_I:
        keyCode = AKEYCODE_I;
        break;
    case Qt::Key_J:
        keyCode = AKEYCODE_J;
        break;
    case Qt::Key_K:
        keyCode = AKEYCODE_K;
        break;
    case Qt::Key_L:
        keyCode = AKEYCODE_L;
        break;
    case Qt::Key_M:
        keyCode = AKEYCODE_M;
        break;
    case Qt::Key_N:
        keyCode = AKEYCODE_N;
        break;
    case Qt::Key_O:
        keyCode = AKEYCODE_O;
        break;
    case Qt::Key_P:
        keyCode = AKEYCODE_P;
        break;
    case Qt::Key_Q:
        keyCode = AKEYCODE_Q;
        break;
    case Qt::Key_R:
        keyCode = AKEYCODE_R;
        break;
    case Qt::Key_S:
        keyCode = AKEYCODE_S;
        break;
    case Qt::Key_T:
        keyCode = AKEYCODE_T;
        break;
    case Qt::Key_U:
        keyCode = AKEYCODE_U;
        break;
    case Qt::Key_V:
        keyCode = AKEYCODE_V;
        break;
    case Qt::Key_W:
        keyCode = AKEYCODE_W;
        break;
    case Qt::Key_X:
        keyCode = AKEYCODE_X;
        break;
    case Qt::Key_Y:
        keyCode = AKEYCODE_Y;
        break;
    case Qt::Key_Z:
        keyCode = AKEYCODE_Z;
        break;
    case Qt::Key_0:
        keyCode = AKEYCODE_0;
        break;
    case Qt::Key_1:
    case Qt::Key_Exclam: // !
        keyCode = AKEYCODE_1;
        break;
    case Qt::Key_2:
        keyCode = AKEYCODE_2;
        break;
    case Qt::Key_3:
        keyCode = AKEYCODE_3;
        break;
    case Qt::Key_4:
    case Qt::Key_Dollar: //$
        keyCode = AKEYCODE_4;
        break;
    case Qt::Key_5:
    case Qt::Key_Percent: // %
        keyCode = AKEYCODE_5;
        break;
    case Qt::Key_6:
    case Qt::Key_AsciiCircum: //^
        keyCode = AKEYCODE_6;
        break;
    case Qt::Key_7:
    case Qt::Key_Ampersand: //&
        keyCode = AKEYCODE_7;
        break;
    case Qt::Key_8:
        keyCode = AKEYCODE_8;
        break;
    case Qt::Key_9:
        keyCode = AKEYCODE_9;
        break;
    case Qt::Key_Space:
        keyCode = AKEYCODE_SPACE;
        break;
    case Qt::Key_Comma: //,
    case Qt::Key_Less:  //<
        keyCode = AKEYCODE_COMMA;
        break;
    case Qt::Key_Period:  //.
    case Qt::Key_Greater: //>
        keyCode = AKEYCODE_PERIOD;
        break;
    case Qt::Key_Minus:      //-
    case Qt::Key_Underscore: //_
        keyCode = AKEYCODE_MINUS;
        break;
    case Qt::Key_Equal: //=
        keyCode = AKEYCODE_EQUALS;
        break;
    case Qt::Key_BracketLeft: //[
    case Qt::Key_BraceLeft:   //{
        keyCode = AKEYCODE_LEFT_BRACKET;
        break;
    case Qt::Key_BracketRight: //]
    case Qt::Key_BraceRight:   //}
        keyCode = AKEYCODE_RIGHT_BRACKET;
        break;
    case Qt::Key_Backslash: // \ ????
    case Qt::Key_Bar:       //|
        keyCode = AKEYCODE_BACKSLASH;
        break;
    case Qt::Key_Semicolon: //;
    case Qt::Key_Colon:     //:
        keyCode = AKEYCODE_SEMICOLON;
        break;
    case Qt::Key_Apostrophe: //'
    case Qt::Key_QuoteDbl:   //"
        keyCode = AKEYCODE_APOSTROPHE;
        break;
    case Qt::Key_Slash:    // /
    case Qt::Key_Question: //?
        keyCode = AKEYCODE_SLASH;
        break;
    case Qt::Key_At: //@
        keyCode = AKEYCODE_AT;
        break;
    case Qt::Key_Plus: //+
        keyCode = AKEYCODE_PLUS;
        break;
    case Qt::Key_QuoteLeft:  //`
    case Qt::Key_AsciiTilde: //~
        keyCode = AKEYCODE_GRAVE;
        break;
    case Qt::Key_NumberSign: //#
        keyCode = AKEYCODE_POUND;
        break;
    case Qt::Key_ParenLeft: //(
        keyCode = AKEYCODE_NUMPAD_LEFT_PAREN;
        break;
    case Qt::Key_ParenRight: //)
        keyCode = AKEYCODE_NUMPAD_RIGHT_PAREN;
        break;
    case Qt::Key_Asterisk: //*
        keyCode = AKEYCODE_STAR;
        break;
    }
    return keyCode;
}

AndroidMetastate InputConvertNormal::convertMetastate(Qt::KeyboardModifiers modifiers)
{
    int metastate = AMETA_NONE;

    if (modifiers & Qt::ShiftModifier) {
        metastate |= AMETA_SHIFT_ON;
    }
    if (modifiers & Qt::ControlModifier) {
        metastate |= AMETA_CTRL_ON;
    }
    if (modifiers & Qt::AltModifier) {
        metastate |= AMETA_ALT_ON;
    }
    if (modifiers & Qt::MetaModifier) {
        metastate |= AMETA_META_ON;
    }
    /*
    if (mod & KMOD_NUM) {
        metastate |= AMETA_NUM_LOCK_ON;
    }
    if (mod & KMOD_CAPS) {
        metastate |= AMETA_CAPS_LOCK_ON;
    }
    if (mod & KMOD_MODE) { // Alt Gr
        // no mapping?
    }
    */
    return static_cast<AndroidMetastate>(metastate);
}
