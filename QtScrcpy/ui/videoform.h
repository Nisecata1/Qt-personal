#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QFileSystemWatcher>
#include <QElapsedTimer>
#include <QKeySequence>
#include <QPointF>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>
#include <QWidget>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QLineEdit;
class QShortcut;
class QYUVOpenGLWidget;
class QLabel;
class KeymapEditorDocument;
class KeymapEditorOverlay;
class KeymapEditorPanel;
class VideoForm : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit VideoForm(bool framelessWindow = false, bool skin = true, bool showToolBar = true, QWidget *parent = 0);
    ~VideoForm();

    void staysOnTop(bool top = true);
    void updateShowSize(const QSize &newSize);
    void updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV);
    void setSerial(const QString& serial);
    void setInitialOrientationHint(int orientation);
    void setLocalTextInputConfig(bool enabled, const QKeySequence &shortcut);
    void setKeymapEditorShortcut(const QKeySequence &shortcut);
    void setScriptBinding(const QString &filePath, const QString &displayName, const QString &json);
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

signals:
    void restartServiceRequested(const QString &serial);

private:
    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);
    void initUI();
    void loadVideoEnabledConfig();
    void updateNoVideoOverlay();
    void applyTheme();
    void reloadViewControlSeparationConfig();
    void applyVideoCanvasLayout();
    void resetOrientationProbeState();
    void resetOrientationProbeTask();
    void initOrientationPoller();
    void startOrientationPollingIfNeeded();
    void stopOrientationPolling();
    void probeOrientationAsync();
    void startNextOrientationProbeStep();
    void advanceOrientationProbeStep();
    void onOrientationProbeStepTimeout();
    void onOrientationProbeBudgetTimeout();
    void onOrientationProbeProcessError(QProcess::ProcessError error);
    void applyResolvedOrientation(int orientation);
    void handleOrientationProbeFinished(int exitCode, QProcess::ExitStatus status);
    static bool parseSurfaceOrientationFromText(const QString &text, int &orientationOut);
    QSize eventFrameSize() const;
    QSize eventShowSize() const;
    void initAiUdpReceiver();
    void onAiUdpReadyRead();
    void centerCursorToVideoFrame();
    bool canUseLocalTextInput() const;
    void positionLocalTextInput();
    void showLocalTextInputOverlay();
    void hideLocalTextInputOverlay(bool restoreVideoFocus, bool clearText = true);
    void submitLocalTextInputOverlay();
    void releaseGrabbedCursorState();
    void initRelativeLookConfigWatcher();
    void ensureRelativeLookConfigWatchPath();
    void reloadRelativeLookInputConfig();
    void setRawInputActive(bool active);
    void dispatchRawInputMouseMove(bool forceSend = false);
    void ensureKeymapEditorUi();
    void positionKeymapEditorUi();
    QRect defaultKeymapEditorPanelGeometry() const;
    QRect normalizeKeymapEditorPanelGeometry(const QRect &requested) const;
    void restoreKeymapEditorPanelGeometry();
    void saveKeymapEditorPanelGeometry();
    void setKeymapEditorActive(bool active);
    void enterKeymapEditor();
    void shutdownKeymapEditor(bool promptForUnsavedChanges);
    bool saveAndApplyKeymapEditor();
    void updateKeymapEditorShortcutStates();
    bool isKeymapEditorActive() const;
    void bindDeviceRecordingState();
    void refreshToolFormRecordingState();
    QString currentRecordFormat() const;
    void startRecordingFromToolForm();
    void stopRecordingFromToolForm();
    void handleRecordingStateChanged(const QString &serial, bool active, const QString &filePath);
    void handleRecordingError(const QString &serial, const QString &message);

    void showToolForm(bool show = true);
    void moveCenter();
    void installShortcut();
    QRect getScreenRect();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
#if defined(Q_OS_WIN32)
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

private:
    // ui
    Ui::videoForm *ui;
    QPointer<ToolForm> m_toolForm;
    QPointer<QWidget> m_loadingWidget;
    QPointer<QYUVOpenGLWidget> m_videoWidget;
    QPointer<QLabel> m_fpsLabel;
    QPointer<QLabel> m_noVideoLabel;
    QPointer<QLineEdit> m_localTextInput;
    QPointer<QShortcut> m_localTextInputShortcut;
    QVector<QPointer<QShortcut>> m_standardShortcuts;
    QPointer<QShortcut> m_toggleKeymapEditorShortcut;
    QPointer<KeymapEditorDocument> m_keymapEditorDocument;
    QPointer<KeymapEditorOverlay> m_keymapEditorOverlay;
    QPointer<KeymapEditorPanel> m_keymapEditorPanel;

    //inside member
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;
    QString m_scriptFilePath;
    QString m_scriptDisplayName;
    QString m_lastAppliedScriptJson;
    QPointer<qsc::IDevice> m_boundDevice;
    bool m_recordingActive = false;
    QString m_recordingFilePath;

    //Whether to display the toolbar when connecting a device.
    bool show_toolbar = true;
    bool m_videoEnabled = true;
    bool m_controlMapToScreen = false;
    int m_videoCenterCropSize = 0;
    int m_lockDirectionIndex = 0;
    bool m_videoSessionFirstFrameLogged = false;
    bool m_pendingVideoWidgetReveal = false;
    QSize m_streamFrameSize;
    QRect m_contentRect;
    QTimer *m_orientationPollTimer = nullptr;
    QPointer<QTimer> m_orientationProbeStepTimer;
    QPointer<QTimer> m_orientationProbeBudgetTimer;
    QPointer<QProcess> m_orientationProbeProcess;
    bool m_orientationProbeBusy = false;
    int m_orientationProbeStepIndex = -1;
    QString m_orientationProbeCurrentSource;
    QElapsedTimer m_orientationProbeTotalElapsed;
    QElapsedTimer m_orientationProbeStepElapsed;
    QSize m_orientationBaseUiSize;
    int m_orientationBaseValue = -1;
    bool m_orientationBaseReady = false;
    int m_pendingInitialOrientation = -1;
    bool m_localTextInputEnabled = true;
    QKeySequence m_localTextInputKeySequence = QKeySequence(QStringLiteral("Ctrl+Shift+T"));
    QKeySequence m_keymapEditorKeySequence = QKeySequence(QStringLiteral("Ctrl+E"));

    bool m_cursorGrabbed = false;
    bool m_rawInputEnabled = true;
    bool m_rawInputRegistered = false;
    bool m_rawInputActive = false;
    int m_rawInputSendHz = 240;
    double m_rawInputScale = 12.0;
    double m_recoilStrength = 0.0;
    bool m_leftButtonDown = false;
    QPointF m_rawInputAccumDelta = QPointF(0.0, 0.0);
    QPointF m_aiRawInputAccumDelta = QPointF(0.0, 0.0);
    QPointF m_rawInputVirtualPos = QPointF(0.0, 0.0);
    QTimer *m_rawInputSendTimer = nullptr;
    QPointer<QUdpSocket> m_aiUdpSocket;

    QFileSystemWatcher *m_relativeLookConfigWatcher = nullptr;
    QTimer *m_relativeLookConfigDebounceTimer = nullptr;
    QString m_relativeLookConfigPath;
    QString m_relativeLookConfigDirPath;
    qint64 m_relativeLookConfigLastModifiedMs = -2;
    bool m_relativeLookConfigLoaded = false;
    bool m_keymapEditorActive = false;
};

#endif // VIDEOFORM_H

