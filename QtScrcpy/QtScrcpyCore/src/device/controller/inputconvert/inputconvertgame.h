#ifndef INPUTCONVERTGAME_H
#define INPUTCONVERTGAME_H

#include <QPoint>
#include <QPointF>
#include <QQueue>
#include <QList>
#include <QString>
#include <QDateTime>
#include <QtGlobal>

#include "inputconvertnormal.h"
#include "keymap.h"

#define MULTI_TOUCH_MAX_NUM 10
class InputConvertGame : public InputConvertNormal
{
    Q_OBJECT
public:
    InputConvertGame(Controller *controller);
    virtual ~InputConvertGame();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void resetInputState() override;
    virtual bool isCurrentCustomKeymap();
    virtual bool useNormalCursorFeedback() const override;
    virtual bool useThrottledNormalCursorFeedback() const override;

    void loadKeyMap(const QString &json);

protected:
    void updateSize(const QSize &frameSize, const QSize &showSize);
    void sendTouchDownEvent(int id, QPointF pos);
    void sendTouchMoveEvent(int id, QPointF pos);
    void sendTouchUpEvent(int id, QPointF pos);
    void sendTouchEvent(int id, QPointF pos, AndroidMotioneventAction action);
    void sendKeyEvent(AndroidKeyeventAction action, AndroidKeycode keyCode);
    QPointF calcFrameAbsolutePos(QPointF relativePos);
    QPointF calcScreenAbsolutePos(QPointF relativePos);

    // multi touch id
    int attachTouchID(int key);
    void detachTouchID(int key);
    int getTouchID(int key);

    // steer wheel
    void processSteerWheel(const KeyMap::KeyMapNode &node, const QKeyEvent *from);

    // click
    void processKeyClick(const QPointF &clickPos, bool clickTwice, bool switchMap, const QKeyEvent *from);

    // click mutil
    void processKeyClickMulti(const KeyMap::DelayClickNode *nodes, const int count, const QKeyEvent *from);

    // drag
    void processKeyDrag(const QPointF &startPos, QPointF endPos, quint32 startDelay, float dragSpeed, const QKeyEvent *from);

    // android key
    void processAndroidKey(AndroidKeycode androidKey, const QKeyEvent *from);

    // mouse
    bool processMouseClick(const QMouseEvent *from);
    bool processMouseMove(const QMouseEvent *from);
    void moveCursorTo(const QMouseEvent *from, const QPoint &localPosPixel);
    void mouseMoveStartTouch(const QMouseEvent *from);
    void mouseMoveStopTouch();
    void startMouseMoveTimer();
    void stopMouseMoveTimer();
    void resetMouseMoveSession(bool releaseTouch);
    void scheduleSmallEyesTouchRebase();

    bool switchGameMap();
    bool isRelativeViewMode() const;
    bool checkCursorPos(const QMouseEvent *from);
    void hideMouseCursor(bool hide);
    void syncRemoteCursorStateForViewMode(bool relativeViewMode);

    void getDelayQueue(const QPointF& start, const QPointF& end,
                       const double& distanceStep, const double& posStepconst,
                       quint32 lowestTimer, quint32 highestTimer,
                       QQueue<QPointF>& queuePos, QQueue<quint32>& queueTimer);
    void getSteerWheelRecoveryQueue(const QPointF &start, const QPointF &target,
                                    QQueue<QPointF> &queuePos, QQueue<quint32> &queueTimer);
    void reloadSteerWheelRecoveryConfigIfNeeded();
    void reloadRelativeLookConfigIfNeeded();
    QPoint calcTouchPacketPos(QPointF pos, bool useLogicalCoords);
    QSize calcTouchPacketSize(bool useLogicalCoords);
    bool calcRelativeLookMovePacketPos(int id, QPointF pos, QPoint &packetPos);
    void resetTouchPacketState(int id);
    void resetRelativeLookTouchState();
    void releaseInjectedTouchSlot(int id, bool immediate);
    void clearSteerWheelState();
    void clearDragState();

protected:
    void timerEvent(QTimerEvent *event);

private slots:
    void onSteerWheelTimer();
    void onDragTimer();
    void onSmallEyesStopTimer();
    void onSmallEyesStartTimer();

private:
    struct TouchPacketState
    {
        bool lastPacketPosValid = false;
        QPoint lastPacketPos;
        bool relativeAccumInitialized = false;
        qreal lastInputLogicalX = 0.0;
        qreal lastInputLogicalY = 0.0;
        qreal lastSentLogicalX = 0.0;
        qreal lastSentLogicalY = 0.0;
        qreal carryX = 0.0;
        qreal carryY = 0.0;
    };

    enum RelativeLookCoordMode
    {
        RLCM_VIDEO,
        RLCM_LOGICAL
    };

    QSize m_frameSize;
    QSize m_showSize;
    bool m_gameMap = false;
    bool m_needBackMouseMove = false;
    int m_multiTouchID[MULTI_TOUCH_MAX_NUM] = { 0 };
    TouchPacketState m_touchPacketState[MULTI_TOUCH_MAX_NUM];
    KeyMap m_keyMap;

    bool m_processMouseMove = true;
    QTimer *m_smallEyesStopTimer = nullptr;
    QTimer *m_smallEyesStartTimer = nullptr;

    // steer wheel
    struct
    {
        // the first key pressed
        int touchKey = Qt::Key_unknown;
        bool pressedUp = false;
        bool pressedDown = false;
        bool pressedLeft = false;
        bool pressedRight = false;

        // for delay
        struct {
            QPointF currentPos;
            QTimer* timer = nullptr;
            QQueue<QPointF> queuePos;
            QQueue<quint32> queueTimer;
            int pressedNum = 0;
        } delayData;
    } m_ctrlSteerWheel;
    struct
    {
        // [common/SteerWheelRecoverySpeed] 0.1~1.0: bigger is faster.
        double speed = 0.45;
        // [common/SteerWheelRecoveryNoise] 0.0~0.01: noise on middle points.
        double noise = 0.0020;
        // [common/SteerWheelRecoveryFinalNoise] 0.0~0.005: micro-noise on final point.
        double finalNoise = 0.0004;
        QString iniPath;
        qint64 lastCheckMs = 0;
        qint64 lastModifiedMs = -2;
        bool initialized = false;
    } m_steerWheelRecoveryConfig;
    struct
    {
        QString iniPath;
        RelativeLookCoordMode mode = RLCM_LOGICAL;
        int logicalSize = 65535;
        qint64 lastCheckMs = 0;
        qint64 lastModifiedMs = -2;
        bool initialized = false;
    } m_relativeLookConfig;

    // mouse move
    struct
    {
        QPointF lastConverPos;
        QPointF lastPos = { 0.0, 0.0 };
        bool touching = false;
        int timer = 0;
        bool smallEyes = false;
        bool needRebase = true;
        int ignoreCount = 0;
        int smoothFrames = -1;
        QList<QPointF> deltaHistory;

        // --- [鏂板] ---
        QPointF smoothDelta; // 鐢ㄤ簬璁板綍涓婁竴甯х殑骞虫粦鍋忕Щ閲?

    } m_ctrlMouseMove;

    // for drag delay
    struct {
        QPointF currentPos;
        QTimer* timer = nullptr;
        QQueue<QPointF> queuePos;
        QQueue<quint32> queueTimer;
        int pressKey = 0;
    } m_dragDelayData;
};

#endif // INPUTCONVERTGAME_H


