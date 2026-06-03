#include <QDebug>
#include <QCursor>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTimer>
#include <QTime>
#include <QtMath>
#include <QRandomGenerator>

#include <QSettings>
#include <QCoreApplication>

#include "inputconvertgame.h"

#define CURSOR_POS_CHECK 50

namespace {
constexpr qint64 kSteerWheelReloadCheckIntervalMs = 80;
constexpr qint64 kRelativeLookReloadCheckIntervalMs = 200;
constexpr int kRelativeLookLogicalSizeMin = 4096;
constexpr int kRelativeLookLogicalSizeMax = 65535;
constexpr qreal kRawSyntheticGlobalPosSentinel = -1000000.0;

QString resolveUserDataIniPath()
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

QPointF mouseGlobalPos(const QMouseEvent *from)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    return from->globalPos();
#else
    return from->globalPosition();
#endif
}

bool isSyntheticRelativeMoveEvent(const QMouseEvent *from)
{
    if (!from) {
        return false;
    }
    QPointF globalPos = mouseGlobalPos(from);
    return globalPos.x() <= (kRawSyntheticGlobalPosSentinel * 0.5)
        && globalPos.y() <= (kRawSyntheticGlobalPosSentinel * 0.5);
}

double randomOffset(double amplitude)
{
    if (amplitude <= 0.0) {
        return 0.0;
    }
    return QRandomGenerator::global()->bounded(amplitude * 2.0) - amplitude;
}

QPointF clampToUnitRect(const QPointF &pos)
{
    return QPointF(qBound(0.0, pos.x(), 1.0), qBound(0.0, pos.y(), 1.0));
}
} // namespace

InputConvertGame::InputConvertGame(Controller *controller) : InputConvertNormal(controller) {
    m_ctrlSteerWheel.delayData.timer = new QTimer(this);
    m_ctrlSteerWheel.delayData.timer->setSingleShot(true);
    connect(m_ctrlSteerWheel.delayData.timer, &QTimer::timeout, this, &InputConvertGame::onSteerWheelTimer);

    m_smallEyesStopTimer = new QTimer(this);
    m_smallEyesStopTimer->setSingleShot(true);
    connect(m_smallEyesStopTimer, &QTimer::timeout, this, &InputConvertGame::onSmallEyesStopTimer);

    m_smallEyesStartTimer = new QTimer(this);
    m_smallEyesStartTimer->setSingleShot(true);
    connect(m_smallEyesStartTimer, &QTimer::timeout, this, &InputConvertGame::onSmallEyesStartTimer);

    reloadRelativeLookConfigIfNeeded();
}

InputConvertGame::~InputConvertGame()
{
    // 析构前把脚本模式里还活着的虚拟触点和鼠标隐藏状态一起收掉。
    resetInputState();
}

bool InputConvertGame::useNormalCursorFeedback() const
{
    return false;
}

bool InputConvertGame::useThrottledNormalCursorFeedback() const
{
    return InputConvertNormal::useThrottledNormalCursorFeedback() && !isRelativeViewMode();
}

void InputConvertGame::resetInputState()
{
    // 供 Controller 切脚本和 Device 断连前调用，统一释放脚本模式遗留的触点、定时器和隐藏鼠标状态。
    InputConvertNormal::resetInputState();
    resetMouseMoveSession(true);
    clearSteerWheelState();
    clearDragState();
    if (m_smallEyesStopTimer) {
        m_smallEyesStopTimer->stop();
    }
    if (m_smallEyesStartTimer) {
        m_smallEyesStartTimer->stop();
    }

    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; ++i) {
        if (m_multiTouchID[i] != 0) {
            releaseInjectedTouchSlot(i, true);
            m_multiTouchID[i] = 0;
        } else {
            resetTouchPacketState(i);
        }
    }

    m_ctrlMouseMove.smallEyes = false;
    m_needBackMouseMove = false;
    m_gameMap = false;
    hideMouseCursor(false);
}

void InputConvertGame::mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (!from) {
        return;
    }

    if (m_keyMap.isSwitchOnKeyboard() == false && m_keyMap.getSwitchKey() == static_cast<int>(from->button())) {
        if (from->type() != QEvent::MouseButtonPress) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    const bool inGameRelativeMode = isRelativeViewMode();
    if (inGameRelativeMode) {
        InputConvertNormal::sendCursorHideEvent();
    } else if (from->type() == QEvent::MouseMove) {
        InputConvertNormal::handleConfiguredCursorFeedback(from, frameSize, showSize);
    }

    if (inGameRelativeMode) {
        updateSize(frameSize, showSize);
        // mouse move
        if (m_keyMap.isValidMouseMoveMap()) {
            if (processMouseMove(from)) {
                return;
            }
        }
        // mouse click
        if (processMouseClick(from)) {
            return;
        }
    }
    InputConvertNormal::mouseEvent(from, frameSize, showSize);
}

void InputConvertGame::wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize)
{
    if (m_gameMap) {
        updateSize(frameSize, showSize);
    } else {
        InputConvertNormal::wheelEvent(from, frameSize, showSize);
    }
}

void InputConvertGame::keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize)
{
    // 澶勭悊寮€鍏虫寜閿?
    if (m_keyMap.isSwitchOnKeyboard() && m_keyMap.getSwitchKey() == from->key()) {
        if (QEvent::KeyPress != from->type()) {
            return;
        }
        if (!switchGameMap()) {
            m_needBackMouseMove = false;
        }
        return;
    }

    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeKey(from->key());
    // 澶勭悊鐗规畩鎸夐敭锛氬彲浠ラ噴鏀惧嚭榧犳爣鐨勬寜閿?
    if (m_needBackMouseMove && KeyMap::KMT_CLICK == node.type && node.data.click.switchMap) {
        updateSize(frameSize, showSize);
        // Qt::Key_Tab Qt::Key_M for PUBG mobile
        processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
        return;
    }
    
    // if (m_gameMap) {
    //     updateSize(frameSize, showSize);
    //     if (!from || from->isAutoRepeat()) {
    //         return;
    //     }

    // 銆愪慨鏀规牳蹇冨垽鏂€昏緫銆?
    // 婊¤冻浠ヤ笅浠讳竴鏉′欢鍗宠涓衡€滄父鎴忔ā寮忔寜閿€濊繘琛屽鐞嗭細
    // 1. 鍏ㄥ眬鏄犲皠寮€鍏?(m_gameMap) 宸叉墦寮€
    // 2. 褰撳墠鎸夐敭琚厤缃负鈥滃父椹荤敓鏁堚€?(alwaysActive) 涓旈厤缃湁鏁?
    bool isGameKey = m_gameMap;
    if (!isGameKey && node.type != KeyMap::KMT_INVALID && node.alwaysActive) {
        isGameKey = true;
    }

    // 杩涘垽鏂€昏緫
    if (isGameKey) {
        updateSize(frameSize, showSize);
        if (!from || from->isAutoRepeat()) {
            return;
        }

        // small eyes锛堜笉鍔級
        if (m_keyMap.isValidMouseMoveMap() && from->key() == m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.key) {
            m_ctrlMouseMove.smallEyes = (QEvent::KeyPress == from->type());

            if (QEvent::KeyPress == from->type()) {
                scheduleSmallEyesTouchRebase();
                stopMouseMoveTimer();
            } else {
                if (m_smallEyesStopTimer) {
                    m_smallEyesStopTimer->stop();
                }
                if (m_smallEyesStartTimer) {
                    m_smallEyesStartTimer->stop();
                }

                m_processMouseMove = true;
                if (m_gameMap && !m_needBackMouseMove) {
                    mouseMoveStopTouch();
                    mouseMoveStartTouch(nullptr);
                } else {
                    mouseMoveStopTouch();
                }

                m_ctrlMouseMove.needRebase = true;
                m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);
                m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
                m_ctrlMouseMove.deltaHistory.clear();
            }
            return;
        }

        switch (node.type) {
        // 澶勭悊鏂瑰悜鐩?
        case KeyMap::KMT_STEER_WHEEL:
            processSteerWheel(node, from);
            return;
        // 澶勭悊鏅€氭寜閿?
        case KeyMap::KMT_CLICK:
            processKeyClick(node.data.click.keyNode.pos, false, node.data.click.switchMap, from);
            processAndroidKey(node.data.click.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_TWICE:
            processKeyClick(node.data.clickTwice.keyNode.pos, true, false, from);
            processAndroidKey(node.data.clickTwice.keyNode.androidKey, from);
            return;
        case KeyMap::KMT_CLICK_MULTI:
            processKeyClickMulti(node.data.clickMulti.keyNode.delayClickNodes, node.data.clickMulti.keyNode.delayClickNodesCount, from);
            return;
        case KeyMap::KMT_DRAG:
            processKeyDrag(node.data.drag.keyNode.pos, node.data.drag.keyNode.extendPos,
                           node.data.drag.startDelay, node.data.drag.dragSpeed, from);
            return;
        case KeyMap::KMT_ANDROID_KEY:
            processAndroidKey(node.data.androidKey.keyNode.androidKey, from);
            return; // 娉ㄦ剰鍘熶唬鐮佽繖閲屽彲鑳芥紡浜?return锛屽姞涓婃瘮杈冨ソ
        default:
            break;
        }
    } else {
        InputConvertNormal::keyEvent(from, frameSize, showSize);
    }
}

bool InputConvertGame::isCurrentCustomKeymap()
{
    return m_gameMap;
}

void InputConvertGame::loadKeyMap(const QString &json)
{
    m_keyMap.loadKeyMap(json);
}

void InputConvertGame::updateSize(const QSize &frameSize, const QSize &showSize)
{
    if (showSize != m_showSize) {
        if (m_gameMap && m_keyMap.isValidMouseMoveMap()) {
#ifdef QT_NO_DEBUG
            // show size change, resize grab cursor
            emit grabCursor(isRelativeViewMode());
#endif
        }
    }
    m_frameSize = frameSize;
    m_showSize = showSize;
}

void InputConvertGame::sendTouchDownEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_DOWN);
}

void InputConvertGame::sendTouchMoveEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_MOVE);
}

void InputConvertGame::sendTouchUpEvent(int id, QPointF pos)
{
    sendTouchEvent(id, pos, AMOTION_EVENT_ACTION_UP);
}

void InputConvertGame::sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action)
{
    // 将游戏脚本触控点转换成 Android 控制消息；相对视角触点直接发送，避免高频 MOVE 在 Qt 事件队列里积压。
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        qWarning() << "InputConvertGame::sendTouchEvent invalid id:" << id;
        return;
    }
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        return;
    }
    const int lookTouchId = getTouchID(Qt::ExtraButton24);
    const bool relativeLookTouch = lookTouchId >= 0 && lookTouchId == id;
    const bool useLogicalCoords = relativeLookTouch && m_relativeLookConfig.mode == RLCM_LOGICAL;
    TouchPacketState &packetState = m_touchPacketState[id];
    QPoint packetPos;
    if (useLogicalCoords && AMOTION_EVENT_ACTION_MOVE == action) {
        if (!calcRelativeLookMovePacketPos(id, pos, packetPos)) {
            delete controlMsg;
            return;
        }
    } else {
        packetPos = calcTouchPacketPos(pos, useLogicalCoords);
        if (useLogicalCoords && AMOTION_EVENT_ACTION_DOWN == action) {
            const int logicalMaxIndex = qMax(1, m_relativeLookConfig.logicalSize - 1);
            const qreal currentLogicalX = qBound<qreal>(0.0, pos.x() * logicalMaxIndex, logicalMaxIndex);
            const qreal currentLogicalY = qBound<qreal>(0.0, pos.y() * logicalMaxIndex, logicalMaxIndex);
            packetState.relativeAccumInitialized = true;
            packetState.lastInputLogicalX = currentLogicalX;
            packetState.lastInputLogicalY = currentLogicalY;
            packetState.lastSentLogicalX = packetPos.x();
            packetState.lastSentLogicalY = packetPos.y();
            packetState.carryX = 0.0;
            packetState.carryY = 0.0;
        }
    }
    if (AMOTION_EVENT_ACTION_MOVE == action
        && packetState.lastPacketPosValid
        && packetState.lastPacketPos == packetPos) {
        delete controlMsg;
        return;
    }
    packetState.lastPacketPos = packetPos;
    packetState.lastPacketPosValid = true;
    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(id),
        action,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(packetPos, calcTouchPacketSize(useLogicalCoords)),
        AMOTION_EVENT_ACTION_DOWN == action ? 1.0f : 0.0f);
    if (relativeLookTouch) {
        sendControlMsgImmediately(controlMsg);
    } else {
        sendControlMsg(controlMsg);
    }
    if (AMOTION_EVENT_ACTION_UP == action) {
        resetTouchPacketState(id);
    }
}

void InputConvertGame::sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode) {
    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_KEYCODE);
    if (!controlMsg) {
        return;
    }

    controlMsg->setInjectKeycodeMsgData(action, keyCode, 0, AMETA_NONE);
    sendControlMsg(controlMsg);
}

QPointF InputConvertGame::calcFrameAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_frameSize.width() * relativePos.x());
    absolutePos.setY(m_frameSize.height() * relativePos.y());
    return absolutePos;
}

QPointF InputConvertGame::calcScreenAbsolutePos(QPointF relativePos)
{
    QPointF absolutePos;
    absolutePos.setX(m_showSize.width() * relativePos.x());
    absolutePos.setY(m_showSize.height() * relativePos.y());
    return absolutePos;
}

QPoint InputConvertGame::calcTouchPacketPos(QPointF pos, bool useLogicalCoords)
{
    if (!useLogicalCoords) {
        return calcFrameAbsolutePos(pos).toPoint();
    }

    const int logicalMaxIndex = m_relativeLookConfig.logicalSize - 1;
    int logicalX = qRound(pos.x() * logicalMaxIndex);
    int logicalY = qRound(pos.y() * logicalMaxIndex);
    logicalX = qBound(0, logicalX, logicalMaxIndex);
    logicalY = qBound(0, logicalY, logicalMaxIndex);
    return QPoint(logicalX, logicalY);
}

QSize InputConvertGame::calcTouchPacketSize(bool useLogicalCoords)
{
    if (!useLogicalCoords) {
        return m_frameSize;
    }
    return QSize(m_relativeLookConfig.logicalSize, m_relativeLookConfig.logicalSize);
}
bool InputConvertGame::calcRelativeLookMovePacketPos(int id, QPointF pos, QPoint &packetPos)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        return false;
    }
    TouchPacketState &packetState = m_touchPacketState[id];
    const int logicalMaxIndex = qMax(1, m_relativeLookConfig.logicalSize - 1);
    const qreal currentLogicalX = qBound<qreal>(0.0, pos.x() * logicalMaxIndex, logicalMaxIndex);
    const qreal currentLogicalY = qBound<qreal>(0.0, pos.y() * logicalMaxIndex, logicalMaxIndex);
    if (!packetState.relativeAccumInitialized) {
        packetState.relativeAccumInitialized = true;
        packetState.lastInputLogicalX = currentLogicalX;
        packetState.lastInputLogicalY = currentLogicalY;
        packetState.lastSentLogicalX = qBound<qreal>(0.0, qRound(currentLogicalX), logicalMaxIndex);
        packetState.lastSentLogicalY = qBound<qreal>(0.0, qRound(currentLogicalY), logicalMaxIndex);
        packetState.carryX = 0.0;
        packetState.carryY = 0.0;
        return false;
    }
    const qreal deltaX = currentLogicalX - packetState.lastInputLogicalX;
    const qreal deltaY = currentLogicalY - packetState.lastInputLogicalY;
    packetState.lastInputLogicalX = currentLogicalX;
    packetState.lastInputLogicalY = currentLogicalY;
    packetState.carryX += deltaX;
    packetState.carryY += deltaY;
    qreal wholeStepX = 0.0;
    if (packetState.carryX >= 1.0) {
        wholeStepX = qFloor(packetState.carryX);
    } else if (packetState.carryX <= -1.0) {
        wholeStepX = qCeil(packetState.carryX);
    }
    qreal wholeStepY = 0.0;
    if (packetState.carryY >= 1.0) {
        wholeStepY = qFloor(packetState.carryY);
    } else if (packetState.carryY <= -1.0) {
        wholeStepY = qCeil(packetState.carryY);
    }
    const qreal candidateX = qBound<qreal>(0.0, packetState.lastSentLogicalX + wholeStepX, logicalMaxIndex);
    const qreal candidateY = qBound<qreal>(0.0, packetState.lastSentLogicalY + wholeStepY, logicalMaxIndex);
    const qreal actualStepX = candidateX - packetState.lastSentLogicalX;
    const qreal actualStepY = candidateY - packetState.lastSentLogicalY;
    if (actualStepX == 0.0) {
        if (candidateX <= 0.0 && packetState.carryX < 0.0) {
            packetState.carryX = 0.0;
        } else if (candidateX >= logicalMaxIndex && packetState.carryX > 0.0) {
            packetState.carryX = 0.0;
        }
    }
    if (actualStepY == 0.0) {
        if (candidateY <= 0.0 && packetState.carryY < 0.0) {
            packetState.carryY = 0.0;
        } else if (candidateY >= logicalMaxIndex && packetState.carryY > 0.0) {
            packetState.carryY = 0.0;
        }
    }
    if (actualStepX == 0.0 && actualStepY == 0.0) {
        return false;
    }
    packetState.lastSentLogicalX = candidateX;
    packetState.lastSentLogicalY = candidateY;
    packetState.carryX -= actualStepX;
    packetState.carryY -= actualStepY;
    packetPos = QPoint(
        static_cast<int>(packetState.lastSentLogicalX),
        static_cast<int>(packetState.lastSentLogicalY));
    return true;
}

void InputConvertGame::resetTouchPacketState(int id)
{
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        return;
    }
    m_touchPacketState[id] = TouchPacketState();
}

void InputConvertGame::resetRelativeLookTouchState()
{
    const int lookTouchId = getTouchID(Qt::ExtraButton24);
    if (lookTouchId >= 0) {
        resetTouchPacketState(lookTouchId);
    }
}

void InputConvertGame::releaseInjectedTouchSlot(int id, bool immediate)
{
    // 按触点槽位直接补发一个 UP，给切脚本、断开连接这类“没有正常按键释放事件”的场景兜底。
    if (0 > id || MULTI_TOUCH_MAX_NUM - 1 < id) {
        return;
    }

    const TouchPacketState &packetState = m_touchPacketState[id];
    if (!packetState.lastPacketPosValid) {
        resetTouchPacketState(id);
        return;
    }

    const bool useLogicalCoords = m_multiTouchID[id] == Qt::ExtraButton24
        && m_relativeLookConfig.mode == RLCM_LOGICAL;

    ControlMsg *controlMsg = new ControlMsg(ControlMsg::CMT_INJECT_TOUCH);
    if (!controlMsg) {
        resetTouchPacketState(id);
        return;
    }

    controlMsg->setInjectTouchMsgData(
        static_cast<quint64>(id),
        AMOTION_EVENT_ACTION_UP,
        static_cast<AndroidMotioneventButtons>(0),
        static_cast<AndroidMotioneventButtons>(0),
        QRect(packetState.lastPacketPos, calcTouchPacketSize(useLogicalCoords)),
        0.0f);
    if (immediate) {
        sendControlMsgImmediately(controlMsg);
    } else {
        sendControlMsg(controlMsg);
    }
    resetTouchPacketState(id);
}

void InputConvertGame::clearSteerWheelState()
{
    // 只收方向轮自己的定时器和按键状态；真正的 UP 统一交给 resetInputState() 里的槽位遍历去补。
    if (m_ctrlSteerWheel.delayData.timer) {
        m_ctrlSteerWheel.delayData.timer->stop();
    }
    m_ctrlSteerWheel.delayData.queuePos.clear();
    m_ctrlSteerWheel.delayData.queueTimer.clear();
    m_ctrlSteerWheel.delayData.currentPos = QPointF();
    m_ctrlSteerWheel.delayData.pressedNum = 0;
    m_ctrlSteerWheel.touchKey = Qt::Key_unknown;
    m_ctrlSteerWheel.pressedUp = false;
    m_ctrlSteerWheel.pressedDown = false;
    m_ctrlSteerWheel.pressedLeft = false;
    m_ctrlSteerWheel.pressedRight = false;
}

void InputConvertGame::clearDragState()
{
    // 收掉拖拽脚本自己的延迟队列和 QTimer，避免对象销毁后还有旧拖拽回调排着执行。
    if (m_dragDelayData.timer) {
        m_dragDelayData.timer->stop();
        delete m_dragDelayData.timer;
        m_dragDelayData.timer = nullptr;
    }
    m_dragDelayData.queuePos.clear();
    m_dragDelayData.queueTimer.clear();
    m_dragDelayData.currentPos = QPointF();
    m_dragDelayData.pressKey = 0;
}
int InputConvertGame::attachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (0 == m_multiTouchID[i]) {
            m_multiTouchID[i] = key;
            return i;
        }
    }
    return -1;
}

void InputConvertGame::detachTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            m_multiTouchID[i] = 0;
            return;
        }
    }
}

int InputConvertGame::getTouchID(int key)
{
    for (int i = 0; i < MULTI_TOUCH_MAX_NUM; i++) {
        if (key == m_multiTouchID[i]) {
            return i;
        }
    }
    return -1;
}

// -------- steer wheel event --------

void InputConvertGame::getDelayQueue(const QPointF& start, const QPointF& end,
                                     const double& distanceStep, const double& posStepconst,
                                     quint32 lowestTimer, quint32 highestTimer,
                                     QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer) {
    double x1 = start.x();
    double y1 = start.y();
    double x2 = end.x();
    double y2 = end.y();

    double dx=x2-x1;
    double dy=y2-y1;
    double e=(fabs(dx)>fabs(dy))?fabs(dx):fabs(dy);
    e /= distanceStep;
    dx/=e;
    dy/=e;

    QQueue<QPointF> queue;
    QQueue<quint32> queue2;
    for(int i=1;i<=e;i++) {
        QPointF pos(x1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst), y1+(QRandomGenerator::global()->bounded(posStepconst*2)-posStepconst));
        queue.enqueue(pos);
        queue2.enqueue(QRandomGenerator::global()->bounded(lowestTimer, highestTimer));
        x1+=dx;
        y1+=dy;
    }

    queuePos = queue;
    queueTimer = queue2;
}

void InputConvertGame::reloadSteerWheelRecoveryConfigIfNeeded()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_steerWheelRecoveryConfig.initialized
        && nowMs - m_steerWheelRecoveryConfig.lastCheckMs < kSteerWheelReloadCheckIntervalMs) {
        return;
    }
    m_steerWheelRecoveryConfig.lastCheckMs = nowMs;

    if (m_steerWheelRecoveryConfig.iniPath.isEmpty()) {
        m_steerWheelRecoveryConfig.iniPath = resolveUserDataIniPath();
    }

    QFileInfo fileInfo(m_steerWheelRecoveryConfig.iniPath);
    const qint64 modifiedMs = fileInfo.exists() ? fileInfo.lastModified().toMSecsSinceEpoch() : -1;
    if (m_steerWheelRecoveryConfig.initialized && modifiedMs == m_steerWheelRecoveryConfig.lastModifiedMs) {
        return;
    }

    QSettings settings(m_steerWheelRecoveryConfig.iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    // [common] SteerWheelRecoverySpeed: 0.1~1.0, bigger value means faster recovery.
    const double speed = settings.value("common/SteerWheelRecoverySpeed", 0.45).toDouble();
    // [common] SteerWheelRecoveryNoise: 0.0~0.01, noise for middle interpolation points.
    const double noise = settings.value("common/SteerWheelRecoveryNoise", 0.0020).toDouble();
    // [common] SteerWheelRecoveryFinalNoise: 0.0~0.005, micro-noise for final point.
    const double finalNoise = settings.value("common/SteerWheelRecoveryFinalNoise", 0.0004).toDouble();

    m_steerWheelRecoveryConfig.speed = qBound(0.1, speed, 1.0);
    m_steerWheelRecoveryConfig.noise = qBound(0.0, noise, 0.01);
    m_steerWheelRecoveryConfig.finalNoise = qBound(0.0, finalNoise, 0.005);
    m_steerWheelRecoveryConfig.lastModifiedMs = modifiedMs;
    m_steerWheelRecoveryConfig.initialized = true;
}

void InputConvertGame::reloadRelativeLookConfigIfNeeded()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (m_relativeLookConfig.initialized
        && nowMs - m_relativeLookConfig.lastCheckMs < kRelativeLookReloadCheckIntervalMs) {
        return;
    }
    m_relativeLookConfig.lastCheckMs = nowMs;

    if (m_relativeLookConfig.iniPath.isEmpty()) {
        m_relativeLookConfig.iniPath = resolveUserDataIniPath();
    }

    QFileInfo fileInfo(m_relativeLookConfig.iniPath);
    const qint64 modifiedMs = fileInfo.exists() ? fileInfo.lastModified().toMSecsSinceEpoch() : -1;
    if (m_relativeLookConfig.initialized && modifiedMs == m_relativeLookConfig.lastModifiedMs) {
        return;
    }

    QSettings settings(m_relativeLookConfig.iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    const RelativeLookCoordMode previousMode = m_relativeLookConfig.mode;
    const int previousLogicalSize = m_relativeLookConfig.logicalSize;
    const bool wasInitialized = m_relativeLookConfig.initialized;

    QString mode = settings.value("common/RelativeLookCoordMode", "logical").toString().trimmed().toLower();
    m_relativeLookConfig.mode = (mode == "video") ? RLCM_VIDEO : RLCM_LOGICAL;

    bool logicalSizeOk = false;
    int logicalSize = settings.value("common/RelativeLookLogicalSize", 65535).toInt(&logicalSizeOk);
    if (!logicalSizeOk) {
        logicalSize = 65535;
    }
    m_relativeLookConfig.logicalSize = qBound(kRelativeLookLogicalSizeMin, logicalSize, kRelativeLookLogicalSizeMax);
    m_relativeLookConfig.lastModifiedMs = modifiedMs;
    m_relativeLookConfig.initialized = true;

    if (!wasInitialized
        || previousMode != m_relativeLookConfig.mode
        || previousLogicalSize != m_relativeLookConfig.logicalSize) {
        qInfo() << "RelativeLook config updated:"
                << "mode=" << (m_relativeLookConfig.mode == RLCM_LOGICAL ? "logical" : "video")
                << "logicalSize=" << m_relativeLookConfig.logicalSize;
    }
}

void InputConvertGame::getSteerWheelRecoveryQueue(const QPointF &start, const QPointF &target,
                                                  QQueue<QPointF> &queuePos, QQueue<quint32> &queueTimer)
{
    queuePos.clear();
    queueTimer.clear();

    const double dx = target.x() - start.x();
    const double dy = target.y() - start.y();
    const double distance = qMax(qAbs(dx), qAbs(dy));
    if (distance <= 0.000001) {
        return;
    }

    const double speedRate = (m_steerWheelRecoveryConfig.speed - 0.1) / 0.9; // normalized to 0~1
    const double distanceStep = 0.006 + speedRate * 0.022;                    // bigger step -> fewer points

    int pointCount = static_cast<int>(qCeil(distance / distanceStep));
    pointCount = qBound(3, pointCount, 120);

    quint32 minTimer = static_cast<quint32>(qRound(7.0 - speedRate * 6.0));   // 7ms -> 1ms
    quint32 maxTimer = static_cast<quint32>(qRound(13.0 - speedRate * 10.0)); // 13ms -> 3ms
    if (maxTimer <= minTimer) {
        maxTimer = minTimer + 1;
    }
    const int timerLow = static_cast<int>(minTimer);
    const int timerHighExclusive = static_cast<int>(maxTimer + 1);

    QQueue<QPointF> points;
    QQueue<quint32> timers;
    for (int i = 1; i <= pointCount; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(pointCount);
        QPointF pos(start.x() + dx * t, start.y() + dy * t);

        // Hybrid model: middle points keep noise, tail points are forced to converge.
        if (i < pointCount - 1) {
            pos.rx() += randomOffset(m_steerWheelRecoveryConfig.noise);
            pos.ry() += randomOffset(m_steerWheelRecoveryConfig.noise);
        } else if (i == pointCount) {
            pos.rx() += randomOffset(m_steerWheelRecoveryConfig.finalNoise);
            pos.ry() += randomOffset(m_steerWheelRecoveryConfig.finalNoise);
        }

        points.enqueue(clampToUnitRect(pos));
        timers.enqueue(static_cast<quint32>(QRandomGenerator::global()->bounded(timerLow, timerHighExclusive)));
    }

    queuePos = points;
    queueTimer = timers;
}

void InputConvertGame::onSteerWheelTimer() {
    if(m_ctrlSteerWheel.delayData.queuePos.empty()) {
        return;
    }

    int id = getTouchID(m_ctrlSteerWheel.touchKey);
    if (id < 0) {
        m_ctrlSteerWheel.delayData.queuePos.clear();
        m_ctrlSteerWheel.delayData.queueTimer.clear();
        return;
    }

    m_ctrlSteerWheel.delayData.currentPos = m_ctrlSteerWheel.delayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_ctrlSteerWheel.delayData.currentPos);

    if(m_ctrlSteerWheel.delayData.queuePos.empty() && m_ctrlSteerWheel.delayData.pressedNum == 0) {
        sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
        detachTouchID(m_ctrlSteerWheel.touchKey);
        m_ctrlSteerWheel.touchKey = Qt::Key_unknown;
        return;
    }

    if(!m_ctrlSteerWheel.delayData.queuePos.empty()) {
        const quint32 nextTimer = m_ctrlSteerWheel.delayData.queueTimer.empty()
                                      ? 1
                                      : m_ctrlSteerWheel.delayData.queueTimer.dequeue();
        m_ctrlSteerWheel.delayData.timer->start(nextTimer);
    }
}

void InputConvertGame::processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from)
{
    reloadSteerWheelRecoveryConfigIfNeeded();

    int key = from->key();
    bool flag = from->type() == QEvent::KeyPress;
    // identify keys
    if (key == node.data.steerWheel.up.key) {
        m_ctrlSteerWheel.pressedUp = flag;
    } else if (key == node.data.steerWheel.right.key) {
        m_ctrlSteerWheel.pressedRight = flag;
    } else if (key == node.data.steerWheel.down.key) {
        m_ctrlSteerWheel.pressedDown = flag;
    } else if (key == node.data.steerWheel.left.key) {
        m_ctrlSteerWheel.pressedLeft = flag;
    } else {
        return;
    }

    // calc offset and pressed number
    QPointF offset(0.0, 0.0);
    int pressedNum = 0;
    if (m_ctrlSteerWheel.pressedUp) {
        ++pressedNum;
        offset.ry() -= node.data.steerWheel.up.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedRight) {
        ++pressedNum;
        offset.rx() += node.data.steerWheel.right.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedDown) {
        ++pressedNum;
        offset.ry() += node.data.steerWheel.down.extendOffset;
    }
    if (m_ctrlSteerWheel.pressedLeft) {
        ++pressedNum;
        offset.rx() -= node.data.steerWheel.left.extendOffset;
    }
    m_ctrlSteerWheel.delayData.pressedNum = pressedNum;
    const QPointF targetPos = node.data.steerWheel.centerPos + offset;

    // last key release and timer no active, active timer to detouch
    if (pressedNum == 0) {
        if (m_ctrlSteerWheel.delayData.timer->isActive()) {
            m_ctrlSteerWheel.delayData.timer->stop();
        }
        m_ctrlSteerWheel.delayData.queueTimer.clear();
        m_ctrlSteerWheel.delayData.queuePos.clear();

        const int id = getTouchID(m_ctrlSteerWheel.touchKey);
        if (id >= 0) {
            sendTouchUpEvent(id, m_ctrlSteerWheel.delayData.currentPos);
        }
        detachTouchID(m_ctrlSteerWheel.touchKey);
        m_ctrlSteerWheel.touchKey = Qt::Key_unknown;
        return;
    }

    // process steer wheel key event
    m_ctrlSteerWheel.delayData.timer->stop();
    m_ctrlSteerWheel.delayData.queueTimer.clear();
    m_ctrlSteerWheel.delayData.queuePos.clear();

    // first active key: attach touch and start from center.
    if (getTouchID(m_ctrlSteerWheel.touchKey) < 0) {
        m_ctrlSteerWheel.touchKey = from->key();
        int id = attachTouchID(m_ctrlSteerWheel.touchKey);
        if (id < 0) {
            return;
        }
        sendTouchDownEvent(id, node.data.steerWheel.centerPos);
        m_ctrlSteerWheel.delayData.currentPos = node.data.steerWheel.centerPos;
    }

    getSteerWheelRecoveryQueue(m_ctrlSteerWheel.delayData.currentPos, targetPos,
                               m_ctrlSteerWheel.delayData.queuePos,
                               m_ctrlSteerWheel.delayData.queueTimer);

    if (!m_ctrlSteerWheel.delayData.queuePos.empty()) {
        const quint32 firstTimer = m_ctrlSteerWheel.delayData.queueTimer.empty()
                                       ? 1
                                       : m_ctrlSteerWheel.delayData.queueTimer.dequeue();
        m_ctrlSteerWheel.delayData.timer->start(firstTimer);
    } else {
        m_ctrlSteerWheel.delayData.currentPos = clampToUnitRect(targetPos);
    }
    return;
}

// -------- key event --------

void InputConvertGame::processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from)
{
    if (switchMap && QEvent::KeyRelease == from->type()) {
        m_needBackMouseMove = !m_needBackMouseMove;
        const bool relativeViewMode = isRelativeViewMode();
        syncRemoteCursorStateForViewMode(relativeViewMode);
        hideMouseCursor(relativeViewMode);
        resetMouseMoveSession(!relativeViewMode);
#ifdef QT_NO_DEBUG
        if (m_keyMap.isValidMouseMoveMap()) {
            emit grabCursor(relativeViewMode);
        }
#endif
    }

    if (QEvent::KeyPress == from->type()) {
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, clickPos);
        if (clickTwice) {
            sendTouchUpEvent(getTouchID(from->key()), clickPos);
            detachTouchID(from->key());
        }
    } else if (QEvent::KeyRelease == from->type()) {
        if (clickTwice) {
            int id = attachTouchID(from->key());
            sendTouchDownEvent(id, clickPos);
        }
        sendTouchUpEvent(getTouchID(from->key()), clickPos);
        detachTouchID(from->key());
    }
}

void InputConvertGame::processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from)
{
    if (QEvent::KeyPress != from->type()) {
        return;
    }

    int key = from->key();
    int delay = 0;
    QPointF clickPos;

    for (int i = 0; i < count; i++) {
        delay += nodes[i].delay;
        clickPos = nodes[i].pos;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = attachTouchID(key);
            sendTouchDownEvent(id, clickPos);
        });

        // Don't up it too fast
        delay += 20;
        QTimer::singleShot(delay, this, [this, key, clickPos]() {
            int id = getTouchID(key);
            sendTouchUpEvent(id, clickPos);
            detachTouchID(key);
        });
    }
}

void InputConvertGame::onDragTimer() {
    if(m_dragDelayData.queuePos.empty()) {
        return;
    }
    int id = getTouchID(m_dragDelayData.pressKey);
    m_dragDelayData.currentPos = m_dragDelayData.queuePos.dequeue();
    sendTouchMoveEvent(id, m_dragDelayData.currentPos);

    if(m_dragDelayData.queuePos.empty()) {
        delete m_dragDelayData.timer;
        m_dragDelayData.timer = nullptr;

        sendTouchUpEvent(id, m_dragDelayData.currentPos);
        detachTouchID(m_dragDelayData.pressKey);

        m_dragDelayData.currentPos = QPointF();
        m_dragDelayData.pressKey = 0;
        return;
    }

    if(!m_dragDelayData.queuePos.empty()) {
        m_dragDelayData.timer->start(m_dragDelayData.queueTimer.dequeue());
    }
}

void InputConvertGame::processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from)
{
    if (QEvent::KeyPress == from->type()) {
        // stop last
        if (m_dragDelayData.timer && m_dragDelayData.timer->isActive()) {
            m_dragDelayData.timer->stop();
            delete m_dragDelayData.timer;
            m_dragDelayData.timer = nullptr;
            m_dragDelayData.queuePos.clear();
            m_dragDelayData.queueTimer.clear();

            sendTouchUpEvent(getTouchID(m_dragDelayData.pressKey), m_dragDelayData.currentPos);
            detachTouchID(m_dragDelayData.pressKey);

            m_dragDelayData.currentPos = QPointF();
            m_dragDelayData.pressKey = 0;
        }

        // start this
        int id = attachTouchID(from->key());
        sendTouchDownEvent(id, startPos);

        m_dragDelayData.timer = new QTimer(this);
        m_dragDelayData.timer->setSingleShot(true);
        connect(m_dragDelayData.timer, &QTimer::timeout, this, &InputConvertGame::onDragTimer);
        m_dragDelayData.pressKey = from->key();
        m_dragDelayData.currentPos = startPos;
        m_dragDelayData.queuePos.clear();
        m_dragDelayData.queueTimer.clear();

        // Clamp dragSpeed to 0-1 range
        const float speed = qBound(0.0f, static_cast<float>(dragSpeed), 1.0f);
        
        // Calculate delays based on dragSpeed
        // dragSpeed = 1 -> minDelay = 1, maxDelay = 2 (fastest)
        // dragSpeed = 0 -> minDelay = 30, maxDelay = 40 (slowest)
        const quint32 minDelay = static_cast<quint32>(1 + (1.0f - speed) * 29);  // 1 to 30
        const quint32 maxDelay = minDelay + static_cast<quint32>((1.0f - speed) * 9) + 1;  // // min + (0 to 9) + 1

        getDelayQueue(startPos, endPos,
                      0.01f, 0.0005f,
                      minDelay,
                      maxDelay,
                      m_dragDelayData.queuePos,
                      m_dragDelayData.queueTimer);

        m_dragDelayData.timer->start(startDelay);
    }
}

void InputConvertGame::processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from)
{
    if (AKEYCODE_UNKNOWN == androidKey) {
        return;
    }

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

    sendKeyEvent(action, androidKey);
}

// -------- mouse event --------

bool InputConvertGame::processMouseClick(const QMouseEvent *from)
{
    const KeyMap::KeyMapNode &node = m_keyMap.getKeyMapNodeMouse(from->button());
    if (KeyMap::KMT_INVALID == node.type) {
        return false;
    }

    if (QEvent::MouseButtonPress == from->type() || QEvent::MouseButtonDblClick == from->type()) {
        int id = attachTouchID(from->button());
        sendTouchDownEvent(id, node.data.click.keyNode.pos);
        return true;
    }
    if (QEvent::MouseButtonRelease == from->type()) {
        int id = getTouchID(from->button());
        sendTouchUpEvent(id, node.data.click.keyNode.pos);
        detachTouchID(from->button());
        return true;
    }
    return false;
}

bool InputConvertGame::processMouseMove(const QMouseEvent *from)
{
    // 防御性编程（Guard Clause）
    // 拿 "鼠标移动事件的标准ID" (QEvent::MouseMove)跟 "当前发生事件的实际ID" (from->type())进行比较
    // QEvent::MouseMove 就是取 QEvent 类(qt的官方类)中定义的常量成员 MouseMove 的值(表示鼠标移动事件的编号, 永远不会更新)
    // -> 用于访问指针所指向类的成员
    if (QEvent::MouseMove != from->type()) {  // 如果 (定义好的标准鼠标移动事件的ID  不等于  当前传入事件from的ID)
        return false;  // 说明这不是一个鼠标移动事件，函数直接返回，不处理
    }

    reloadRelativeLookConfigIfNeeded();

    const bool syntheticRelativeMove = isSyntheticRelativeMoveEvent(from);
    if (!syntheticRelativeMove && checkCursorPos(from)) {
        return true;
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    const QPointF currentPos = from->localPos();
#else
    const QPointF currentPos = from->position();
#endif

    if (m_ctrlMouseMove.ignoreCount > 0) {
        --m_ctrlMouseMove.ignoreCount;
        m_ctrlMouseMove.lastPos = currentPos;
        m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
        m_ctrlMouseMove.deltaHistory.clear();
        return true;
    }

    if (!m_processMouseMove) {
        m_ctrlMouseMove.lastPos = currentPos;
        return true;
    }

    if (m_ctrlMouseMove.needRebase) {
        // Ensure DOWN is sent before any MOVE in the new relative session.
        mouseMoveStartTouch(from);
        startMouseMoveTimer();

        m_ctrlMouseMove.lastPos = currentPos;
        m_ctrlMouseMove.needRebase = false;
        m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
        m_ctrlMouseMove.deltaHistory.clear();
        return true;
    }

    const QPointF lastPos = m_ctrlMouseMove.lastPos;
    m_ctrlMouseMove.lastPos = currentPos;
    const QPointF currentRawDelta = currentPos - lastPos;

    if (m_ctrlMouseMove.smoothFrames < 0) {
        QString iniPath = resolveUserDataIniPath();
        QSettings settings(iniPath, QSettings::IniFormat);
        m_ctrlMouseMove.smoothFrames = settings.value("common/MouseSmoothFrames", 4).toInt();
        m_ctrlMouseMove.smoothFrames = qBound(1, m_ctrlMouseMove.smoothFrames, 20);
        qInfo() << "RelativeLook smoothing configured:" << "frames=" << m_ctrlMouseMove.smoothFrames;
    }

    m_ctrlMouseMove.deltaHistory.append(currentRawDelta);
    while (m_ctrlMouseMove.deltaHistory.size() > m_ctrlMouseMove.smoothFrames) {
        m_ctrlMouseMove.deltaHistory.removeFirst();
    }

    QPointF smoothDelta(0.0, 0.0);
    for (const QPointF &delta : m_ctrlMouseMove.deltaHistory) {
        smoothDelta += delta;
    }
    smoothDelta /= m_ctrlMouseMove.deltaHistory.size();

    m_ctrlMouseMove.smoothDelta = smoothDelta;
    QPointF distance_raw = m_ctrlMouseMove.smoothDelta;

    QPointF speedRatio{m_keyMap.getMouseMoveMap().data.mouseMove.speedRatio};
    QPointF distance{distance_raw.x() / speedRatio.x(), distance_raw.y() / speedRatio.y()};

    const bool useLogicalCoordStep = m_relativeLookConfig.mode == RLCM_LOGICAL;
    const qreal stepBaseX = useLogicalCoordStep
        ? qMax(1, m_relativeLookConfig.logicalSize - 1)
        : qMax(1, m_showSize.width());
    const qreal stepBaseY = useLogicalCoordStep
        ? qMax(1, m_relativeLookConfig.logicalSize - 1)
        : qMax(1, m_showSize.height());

    mouseMoveStartTouch(from);
    startMouseMoveTimer();

    QPointF newConverPos(
        m_ctrlMouseMove.lastConverPos.x() + distance.x() / stepBaseX,
        m_ctrlMouseMove.lastConverPos.y() + distance.y() / stepBaseY
    );
    float minX = 0.1f;
    float maxX = 0.9f;
    float minY = 0.1f;
    float maxY = 0.9f;

    if (newConverPos.x() < minX || newConverPos.x() > maxX
        || newConverPos.y() < minY || newConverPos.y() > maxY) {
        if (m_ctrlMouseMove.smallEyes) {
            scheduleSmallEyesTouchRebase();
        } else {
            mouseMoveStopTouch();
            mouseMoveStartTouch(from);

            QPointF startPos = m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
            m_ctrlMouseMove.lastConverPos.setX(startPos.x() + distance.x() / stepBaseX);
            m_ctrlMouseMove.lastConverPos.setY(startPos.y() + distance.y() / stepBaseY);
            m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
            m_ctrlMouseMove.deltaHistory.clear();
        }
    } else {
        m_ctrlMouseMove.lastConverPos = newConverPos;
    }

    const int touchId = getTouchID(Qt::ExtraButton24);
    if (touchId >= 0) {
        sendTouchMoveEvent(touchId, m_ctrlMouseMove.lastConverPos);
    }

    return true;
}

bool InputConvertGame::checkCursorPos(const QMouseEvent *from)
{
    bool moveCursor = false;
    QPoint pos = from->pos();
    if (pos.x() < CURSOR_POS_CHECK) {
        pos.setX(m_showSize.width() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.x() > m_showSize.width() - CURSOR_POS_CHECK) {
        pos.setX(CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() < CURSOR_POS_CHECK) {
        pos.setY(m_showSize.height() - CURSOR_POS_CHECK);
        moveCursor = true;
    } else if (pos.y() > m_showSize.height() - CURSOR_POS_CHECK) {
        pos.setY(CURSOR_POS_CHECK);
        moveCursor = true;
    }


    if (moveCursor) {
        moveCursorTo(from, pos);
        m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);  // 杩欓噷鍐欐浜?(0,0)
        m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
        m_ctrlMouseMove.needRebase = true;
        m_ctrlMouseMove.deltaHistory.clear();
    }

    return moveCursor;
}

void InputConvertGame::moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel)
{
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QPoint posOffset = from->pos() - localPosPixel;
    QPoint globalPos = from->globalPos();
#else
    QPoint posOffset = from->position().toPoint() - localPosPixel;
    QPoint globalPos = from->globalPosition().toPoint();
#endif
    globalPos -= posOffset;
    //qDebug()<<"move cursor to "<<globalPos<<" offset "<<posOffset;
    QCursor::setPos(globalPos);
}

void InputConvertGame::mouseMoveStartTouch(const QMouseEvent *from)
{
    Q_UNUSED(from)
    if (!m_ctrlMouseMove.touching) {
        QPointF mouseMoveStartPos
            = m_ctrlMouseMove.smallEyes ? m_keyMap.getMouseMoveMap().data.mouseMove.smallEyes.pos : m_keyMap.getMouseMoveMap().data.mouseMove.startPos;
        int id = attachTouchID(Qt::ExtraButton24);
        if (id < 0) {
            return;
        }
        resetTouchPacketState(id);
        sendTouchDownEvent(id, mouseMoveStartPos);
        m_ctrlMouseMove.lastConverPos = mouseMoveStartPos;
        m_ctrlMouseMove.touching = true;
    }
}

void InputConvertGame::mouseMoveStopTouch()
{
    if (m_ctrlMouseMove.touching) {
        const int id = getTouchID(Qt::ExtraButton24);
        if (id >= 0) {
            sendTouchUpEvent(id, m_ctrlMouseMove.lastConverPos);
            resetTouchPacketState(id);
        }
        detachTouchID(Qt::ExtraButton24);
        m_ctrlMouseMove.touching = false;
    }
}

void InputConvertGame::startMouseMoveTimer()
{
    stopMouseMoveTimer();
    m_ctrlMouseMove.timer = startTimer(500);
}

void InputConvertGame::stopMouseMoveTimer()
{
    if (0 != m_ctrlMouseMove.timer) {
        killTimer(m_ctrlMouseMove.timer);
        m_ctrlMouseMove.timer = 0;
    }
}

void InputConvertGame::resetMouseMoveSession(bool releaseTouch)
{
    stopMouseMoveTimer();
    if (m_smallEyesStopTimer) {
        m_smallEyesStopTimer->stop();
    }
    if (m_smallEyesStartTimer) {
        m_smallEyesStartTimer->stop();
    }
    if (releaseTouch) {
        mouseMoveStopTouch();
    }
    resetRelativeLookTouchState();
    m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);
    m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
    m_ctrlMouseMove.ignoreCount = 0;
    m_ctrlMouseMove.needRebase = true;
    m_ctrlMouseMove.deltaHistory.clear();
    m_processMouseMove = true;
}

void InputConvertGame::scheduleSmallEyesTouchRebase()
{
    if (!m_gameMap || m_needBackMouseMove || !m_ctrlMouseMove.smallEyes) {
        m_processMouseMove = true;
        return;
    }

    m_processMouseMove = false;
    if (m_smallEyesStopTimer) {
        m_smallEyesStopTimer->stop();
        m_smallEyesStopTimer->start(30);
    }
    if (m_smallEyesStartTimer) {
        m_smallEyesStartTimer->stop();
        m_smallEyesStartTimer->start(60);
    }
}

void InputConvertGame::onSmallEyesStopTimer()
{
    if (!m_gameMap || m_needBackMouseMove || !m_ctrlMouseMove.smallEyes) {
        return;
    }
    mouseMoveStopTouch();
}

void InputConvertGame::onSmallEyesStartTimer()
{
    if (!m_gameMap || m_needBackMouseMove || !m_ctrlMouseMove.smallEyes) {
        m_processMouseMove = true;
        return;
    }

    mouseMoveStartTouch(nullptr);
    m_ctrlMouseMove.needRebase = true;
    m_ctrlMouseMove.lastPos = QPointF(0.0, 0.0);
    m_ctrlMouseMove.smoothDelta = QPointF(0.0, 0.0);
    m_ctrlMouseMove.deltaHistory.clear();
    m_processMouseMove = true;
}

bool InputConvertGame::switchGameMap()
{
    m_gameMap = !m_gameMap;
    qInfo() << QString("current keymap mode: %1" ).arg(m_gameMap ? "custom" : "normal");

    const bool relativeViewMode = isRelativeViewMode();
    syncRemoteCursorStateForViewMode(relativeViewMode);
    resetMouseMoveSession(!relativeViewMode);

    if (!m_keyMap.isValidMouseMoveMap()) {
        return m_gameMap;
    }
#ifdef QT_NO_DEBUG
    // grab cursor and set cursor only mouse move map
    emit grabCursor(relativeViewMode);
#endif
    hideMouseCursor(relativeViewMode);

    return m_gameMap;
}

bool InputConvertGame::isRelativeViewMode() const
{
    return m_gameMap && !m_needBackMouseMove;
}

void InputConvertGame::syncRemoteCursorStateForViewMode(bool relativeViewMode)
{
    if (relativeViewMode) {
        sendCursorHideEvent();
    } else {
        clearPendingRemoteCursorState();
    }
}

void InputConvertGame::hideMouseCursor(bool hide)
{
    if (hide) {
#ifdef QT_NO_DEBUG
        const QCursor cursor(Qt::BlankCursor);
#else
        const QCursor cursor(Qt::CrossCursor);
#endif
        if (QGuiApplication::overrideCursor()) {
            QGuiApplication::changeOverrideCursor(cursor);
        } else {
            QGuiApplication::setOverrideCursor(cursor);
        }
    } else {
        QGuiApplication::restoreOverrideCursor();
    }
}

void InputConvertGame::timerEvent(QTimerEvent *event)
{
    if (m_ctrlMouseMove.timer == event->timerId()) {
        stopMouseMoveTimer();
        mouseMoveStopTouch();
    }
}







