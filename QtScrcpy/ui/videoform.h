#ifndef VIDEOFORM_H
#define VIDEOFORM_H

#include <QFileSystemWatcher>
#include <QPointF>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

namespace Ui
{
    class videoForm;
}

class ToolForm;
class FileHandler;
class QYUVOpenGLWidget;
class QLabel;
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
    QRect getGrabCursorRect();
    const QSize &frameSize();
    void resizeSquare();
    void removeBlackRect();
    void showFPS(bool show);
    void switchFullScreen();
    bool isHost();

private:
    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;
    void updateFPS(quint32 fps) override;
    void grabCursor(bool grab) override;

    void updateStyleSheet(bool vertical);
    QMargins getMargins(bool vertical);
    void initUI();
    void centerCursorToVideoFrame();
    void initRelativeLookConfigWatcher();
    void ensureRelativeLookConfigWatchPath();
    void reloadRelativeLookInputConfig();
    void setRawInputActive(bool active);
    void dispatchRawInputMouseMove(bool forceSend = false);

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

    //inside member
    QSize m_frameSize;
    QSize m_normalSize;
    QPoint m_dragPosition;
    float m_widthHeightRatio = 0.5f;
    bool m_skin = true;
    QPoint m_fullScreenBeforePos;
    QString m_serial;

    //Whether to display the toolbar when connecting a device.
    bool show_toolbar = true;

    bool m_cursorGrabbed = false;
    bool m_rawInputEnabled = true;
    bool m_rawInputRegistered = false;
    bool m_rawInputActive = false;
    int m_rawInputSendHz = 240;
    double m_rawInputScale = 12.0;
    QPointF m_rawInputAccumDelta = QPointF(0.0, 0.0);
    QPointF m_rawInputVirtualPos = QPointF(0.0, 0.0);
    QTimer *m_rawInputSendTimer = nullptr;

    QFileSystemWatcher *m_relativeLookConfigWatcher = nullptr;
    QTimer *m_relativeLookConfigDebounceTimer = nullptr;
    QString m_relativeLookConfigPath;
    QString m_relativeLookConfigDirPath;
    qint64 m_relativeLookConfigLastModifiedMs = -2;
    bool m_relativeLookConfigLoaded = false;
};

#endif // VIDEOFORM_H
