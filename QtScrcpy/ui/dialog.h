#ifndef DIALOG_H
#define DIALOG_H

#include <QWidget>
#include <QPointer>
#include <QMessageBox>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QListWidget>
#include <QTimer>
#include <QHash>
#include <QKeySequence>


#include "adbprocess.h"
#include "config.h"
#include "../QtScrcpyCore/include/QtScrcpyCore.h"
#include "audio/audiooutput.h"

namespace Ui
{
    class Widget;
}

class QYUVOpenGLWidget;
class VideoForm;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QKeySequenceEdit;
class QLabel;
class QSpinBox;
class QToolButton;
class QWidget;
class Dialog : public QWidget
{
    Q_OBJECT

public:
    explicit Dialog(QWidget *parent = 0);
    ~Dialog();

    void outLog(const QString &log, bool newLine = true);
    bool filterLog(const QString &log);
    void getIPbyIp();

private slots:
    void onDeviceConnected(bool success, const QString& serial, const QString& deviceName, const QSize& size, int initialOrientation = -1);
    void onDeviceDisconnected(QString serial);

    void on_updateDevice_clicked();
    void on_startServerBtn_clicked();
    void on_stopServerBtn_clicked();
    void on_restartServerBtn_clicked();
    void onRestartDeviceRequested(const QString &serial);
    void on_wirelessConnectBtn_clicked();
    void on_startAdbdBtn_clicked();
    void on_getIPBtn_clicked();
    void on_wirelessDisConnectBtn_clicked();
    void on_selectRecordPathBtn_clicked();
    void on_openAppFolderBtn_clicked();
    void on_recordPathEdt_textChanged(const QString &arg1);
    void on_adbCommandBtn_clicked();
    void on_stopAdbBtn_clicked();
    void on_clearOut_clicked();
    void on_stopAllServerBtn_clicked();
    void on_refreshGameScriptBtn_clicked();
    void on_applyScriptBtn_clicked();
    void on_recordScreenCheck_clicked(bool checked);
    void on_localTextInputCheck_toggled(bool checked);
    void on_localTextInputShortcutEdit_keySequenceChanged(const QKeySequence &keySequence);
    void on_keymapEditorShortcutEdit_keySequenceChanged(const QKeySequence &keySequence);
    void on_usbConnectBtn_clicked();
    void on_wifiConnectBtn_clicked();
    void on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item);
    void on_updateNameBtn_clicked();
    void on_useSingleModeCheck_clicked();

    void on_startAudioBtn_clicked();

    void on_stopAudioBtn_clicked();

    void on_installSndcpyBtn_clicked();

    void on_autoUpdatecheckBox_toggled(bool checked);
    void on_autoUpdateIntervalSpin_valueChanged(int value);

    void showIpEditMenu(const QPoint &pos);
    void onSelectedDeviceMouseConfigEdited();
    void onSelectedDeviceCenterCropConfigEdited();
    void onThemeModeChanged(int index);

private:
    bool checkAdbRun();
    void initUI();
    void initGameFeatureUi();
    void initUsbMouseConfigUi();
    void updateBootConfig(bool toView = true);
    void execAdbCmd();
    void delayMs(int ms);
    QString getGameScript(const QString &fileName);
    QString getGameScriptPath(const QString &fileName) const;
    void slotActivated(QSystemTrayIcon::ActivationReason reason);
    int findDeviceFromeSerialBox(bool wifi);
    quint32 getBitRate();
    qsc::DeviceParams buildDeviceParams(const QString &serial);
    const QString &getServerPath();
    void applyLocalTextInputConfigToOpenVideoForms();
    void applyKeymapEditorShortcutToOpenVideoForms();
    void updateVideoFormScriptBinding(const QString &serial, const QString &scriptFilePath, const QString &scriptDisplayName, const QString &scriptJson);
    void loadIpHistory();
    void saveIpHistory(const QString &ip);
    void loadPortHistory();
    void savePortHistory(const QString &port);
    void restartApplication();
    void quitApplicationDirectly();
    void showPortEditMenu(const QPoint &pos);
    void handleSelectedSerialChanged(const QString &serial);
    void initControlToolTips();
    void refreshControlToolTips();
    void updateSelectedDeviceConfigUi(const QString &serial);
    void updateSelectedDeviceConfigControlState();
    void setMouseConfigExpanded(bool expanded);
    void saveSelectedDeviceMouseConfig();
    void saveSelectedDeviceCenterCropConfig();
    int currentAutoUpdateIntervalSec() const;
    int currentAutoUpdateIntervalMs() const;
    void applyAutoUpdateTimerState();
    void refreshAutoUpdateToolTips();
    QString buildRecordPathToolTip() const;
    QString buildLocalTextInputShortcutToolTip() const;
    QString buildKeymapEditorShortcutToolTip() const;
    QString buildDeviceIpToolTip() const;
    QString buildDevicePortToolTip() const;
    QString buildMouseConfigToggleToolTip() const;
    QKeySequence currentKeymapEditorShortcut() const;
    ThemeMode currentThemeModeSelection() const;
    QString currentSelectedSerial() const;
    QString formatSelectedDeviceDisplayName(const QString &serial) const;

protected:
    void closeEvent(QCloseEvent *event);

private:
    Ui::Widget *ui;
    qsc::AdbProcess m_adb;
    QSystemTrayIcon *m_hideIcon;
    QMenu *m_menu;
    QAction *m_showWindow;
    QAction *m_restart;
    QAction *m_quit;
    AudioOutput m_audioOutput;
    QTimer m_autoUpdatetimer;
    QHash<QString, QPointer<VideoForm>> m_videoForms;
    QComboBox *m_themeModeBox = nullptr;
    QGroupBox *m_gameFeatureGroup = nullptr;
    QGroupBox *m_gameDeviceConfigGroup = nullptr;
    QLabel *m_gameSelectedDeviceSerialValue = nullptr;
    QKeySequenceEdit *m_keymapEditorShortcutEdit = nullptr;
    QLabel *m_selectedDeviceSerialValue = nullptr;
    QCheckBox *m_deviceCenterCropCheck = nullptr;
    QSpinBox *m_deviceCenterCropSizeSpin = nullptr;
    QToolButton *m_mouseConfigToggleBtn = nullptr;
    QWidget *m_mouseConfigContent = nullptr;
    QCheckBox *m_renderRemoteCursorCheck = nullptr;
    QSpinBox *m_cursorSizeSpin = nullptr;
    QCheckBox *m_normalMouseCompatEnabledCheck = nullptr;
    QCheckBox *m_normalMouseTouchPriorityEnabledCheck = nullptr;
    QCheckBox *m_normalMouseCursorThrottleEnabledCheck = nullptr;
    QSpinBox *m_normalMouseCursorFlushIntervalSpin = nullptr;
    QSpinBox *m_normalMouseCursorClickSuppressionSpin = nullptr;
    QSpinBox *m_normalMouseTapMinHoldSpin = nullptr;
    bool m_updatingSelectedDeviceConfigUi = false;
};

#endif // DIALOG_H
