#ifndef INPUTCONVERT_H
#define INPUTCONVERT_H

#include <QDateTime>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVariant>

#include "inputconvertbase.h"

class QFileSystemWatcher;
class QTimer;

class InputConvertNormal : public InputConvertBase
{
    Q_OBJECT
public:
    InputConvertNormal(Controller *controller);
    virtual ~InputConvertNormal();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void resetInputState() override;
    virtual bool useNormalCursorFeedback() const;
    virtual bool useThrottledNormalCursorFeedback() const;

protected:
    void handleConfiguredCursorFeedback(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    bool sendCursorPositionEvent(qint32 x, qint32 y, qint32 w, qint32 h);
    bool sendCursorPositionEvent(qint32 x, qint32 y, qint32 w, qint32 h, int priority);
    void sendCursorConfigEvent(qint32 cursorSizePx);
    void sendCursorConfigEvent(qint32 cursorSizePx, int priority);
    void sendCursorHideEvent();
    void sendCursorPositionFromMouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    bool sendRemoteCursorUpdate(const QPoint &remotePos, const QSize &remoteFrameSize, bool sendMouseHover);
    void initCursorConfigWatcher();
    void ensureCursorConfigWatchPath();
    void initRemoteCursorDispatchTimers();
    void reloadNormalMouseCompatConfigIfNeeded();
    QString resolveUserDataIniPath() const;
    void enterRemoteCursorSuppressionWindow();
    void clearPendingRemoteCursorState();
    void queueCursorPositionFromMouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    bool parseBoolSetting(const QVariant &value, bool defaultValue, bool *ok) const;
    bool resolveTouchPosition(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize,
                              QPoint *touchPos) const;
    bool sendTouchEvent(AndroidMotioneventAction action, const QPoint &touchPos, const QSize &frameSize,
                        AndroidMotioneventButtons actionButtons, AndroidMotioneventButtons buttons, quint64 pointerId,
                        bool immediate = false, int priority = Qt::NormalEventPriority, bool trackActiveTouch = true);
    bool hideRemoteCursorImmediately();
    bool useNormalMouseTouchPriority() const;
    bool useRemoteCursorSdkMouseCompat() const;
    quint64 resolveNormalTouchPointerId(AndroidMotioneventAction action) const;

private slots:
    void onCursorConfigFileChanged(const QString &path);
    void onCursorConfigDebounced();
    void onRemoteCursorFlushTimer();
    void onRemoteCursorSuppressionTimeout();
    void onNormalTapReleaseTimeout();

private:
    void applyNormalMouseCompatTimerConfig();
    void beginNormalTapTracking(const QMouseEvent *from);
    bool maybeDelayNormalTapRelease(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    void clearPendingNormalTapRelease();
    void flushPendingNormalTapRelease(bool immediate = false);
    bool resolveRemoteCursorPosition(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize,
                                     QPoint *remotePos, QSize *remoteFrameSize) const;
    void flushPendingRemoteCursor();
    AndroidMotioneventButtons convertMouseButtons(Qt::MouseButtons buttonState);
    AndroidMotioneventButtons convertMouseButton(Qt::MouseButton button);
    AndroidKeycode convertKeyCode(int key, Qt::KeyboardModifiers modifiers);
    AndroidMetastate convertMetastate(Qt::KeyboardModifiers modifiers);

private:
    bool m_remoteCursorEnabled = false;
    qint32 m_cursorSizePx = 24;
    qint32 m_lastSentCursorSizePx = -1;
    bool m_remoteCursorVisible = false;
    QPoint m_lastRemoteCursorPos = QPoint(-1, -1);
    QSize m_lastRemoteCursorFrameSize;
    QString m_cursorConfigPath;
    QFileSystemWatcher *m_cursorConfigWatcher = nullptr;
    QTimer *m_cursorConfigDebounceTimer = nullptr;
    QTimer *m_remoteCursorFlushTimer = nullptr;
    QTimer *m_remoteCursorSuppressionTimer = nullptr;
    QTimer *m_normalTapReleaseTimer = nullptr;
    QPoint m_pendingRemoteCursorPos = QPoint(-1, -1);
    QSize m_pendingRemoteCursorFrameSize;
    bool m_pendingRemoteCursorValid = false;
    bool m_remoteCursorSuppressed = false;
    bool m_remoteCursorThrottleLogPrinted = false;
    struct
    {
        QString iniPath;
        qint64 lastCheckMs = 0;
        qint64 lastModifiedMs = -2;
        bool initialized = false;
        bool enabled = false;
        bool touchPriorityEnabled = true;
        bool cursorThrottleEnabled = true;
        int cursorFlushIntervalMs = 33;
        int clickSuppressionMs = 120;
        int tapMinHoldMs = 16;
        bool remoteCursorImmediate = true;
        int remoteCursorMaxPendingBytes = 1024;
        bool remoteCursorSdkMouseCompatEnabled = false;
    } m_normalMouseCompatConfig;
    int m_remoteCursorDroppedSinceLastLog = 0;
    qint64 m_remoteCursorDropLastLogMs = 0;
    QPoint m_pendingNormalTapReleasePos = QPoint(-1, -1);
    QSize m_pendingNormalTapReleaseFrameSize;
    AndroidMotioneventButtons m_pendingNormalTapReleaseActionButtons = static_cast<AndroidMotioneventButtons>(0);
    AndroidMotioneventButtons m_pendingNormalTapReleaseButtons = static_cast<AndroidMotioneventButtons>(0);
    quint64 m_pendingNormalTapReleasePointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
    bool m_pendingNormalTapReleaseValid = false;
    QPoint m_activeNormalTouchPos = QPoint(-1, -1);
    QSize m_activeNormalTouchFrameSize;
    quint64 m_activeNormalTouchPointerId = static_cast<quint64>(POINTER_ID_GENERIC_FINGER);
    bool m_activeNormalTouchDown = false;
    qint64 m_leftPressStartMs = 0;
    bool m_leftTapCandidate = false;
    bool m_pendingRemoteCursorSendMouseHover = false;
};

#endif // INPUTCONVERT_H
