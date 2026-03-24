#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QApplication>
#include <QLocale>
#include <QProcess>
#include <QRandomGenerator>
#include <QTime>
#include <QTimer>
#include <QKeySequenceEdit>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include "config.h"
#include "thememanager.h"
#include "dialog.h"
#include "ui_dialog.h"
#include "videoform.h"
#include "../groupcontroller/groupcontroller.h"

#ifdef Q_OS_WIN32
#include "../util/winutils.h"
#endif

QString s_keyMapPath = "";

namespace {
void setComboAndLineEditToolTip(QComboBox *comboBox, const QString &toolTip)
{
    if (!comboBox) {
        return;
    }

    comboBox->setToolTip(toolTip);
    if (comboBox->lineEdit()) {
        comboBox->lineEdit()->setToolTip(toolTip);
    }
}

QKeySequence normalizeSingleShortcut(const QKeySequence &shortcut)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (shortcut.isEmpty() || shortcut[0] == QKeyCombination()) {
        return QKeySequence();
    }
    return QKeySequence(shortcut[0]);
#else
    if (shortcut.isEmpty() || shortcut[0] == 0) {
        return QKeySequence();
    }
    return QKeySequence(shortcut[0]);
#endif
}
}

const QString &getKeyMapPath()
{
    if (s_keyMapPath.isEmpty()) {
        s_keyMapPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_KEYMAP_PATH"));
        QFileInfo fileInfo(s_keyMapPath);
        if (s_keyMapPath.isEmpty() || !fileInfo.isDir()) {
            s_keyMapPath = QCoreApplication::applicationDirPath() + "/keymap";
        }
    }
    return s_keyMapPath;
}

Dialog::Dialog(QWidget *parent) : QWidget(parent), ui(new Ui::Widget)
{
    ui->setupUi(this);
    initUI();
    connect(&m_autoUpdatetimer, &QTimer::timeout, this, &Dialog::on_updateDevice_clicked);

    updateBootConfig(true);
    refreshControlToolTips();

    on_useSingleModeCheck_clicked();
    on_updateDevice_clicked();

    connect(&m_adb, &qsc::AdbProcess::adbProcessResult, this, [this](qsc::AdbProcess::ADB_EXEC_RESULT processResult) {
        QString log = "";
        bool newLine = true;
        QStringList args = m_adb.arguments();

        switch (processResult) {
        case qsc::AdbProcess::AER_ERROR_START:
            break;
        case qsc::AdbProcess::AER_SUCCESS_START:
            log = "adb run";
            newLine = false;
            break;
        case qsc::AdbProcess::AER_ERROR_EXEC:
            //log = m_adb.getErrorOut();
            if (args.contains("ifconfig") && args.contains("wlan0")) {
                getIPbyIp();
            }
            break;
        case qsc::AdbProcess::AER_ERROR_MISSING_BINARY:
            log = "adb not found";
            break;
        case qsc::AdbProcess::AER_SUCCESS_EXEC:
            //log = m_adb.getStdOut();
            if (args.contains("devices")) {
                const QString previousSerial = currentSelectedSerial();
                QStringList devices = m_adb.getDevicesSerialFromStdOut();

                int selectedIndex = -1;
                {
                    const QSignalBlocker serialBlocker(ui->serialBox);
                    const QSignalBlocker phoneListBlocker(ui->connectedPhoneList);

                    ui->serialBox->clear();
                    ui->connectedPhoneList->clear();
                    for (auto &item : devices) {
                        ui->serialBox->addItem(item);
                        ui->connectedPhoneList->addItem(Config::getInstance().getNickName(item) + "-" + item);
                    }

                    if (!previousSerial.isEmpty()) {
                        selectedIndex = ui->serialBox->findText(previousSerial);
                    }
                    if (selectedIndex < 0 && ui->serialBox->count() > 0) {
                        selectedIndex = 0;
                    }

                    if (selectedIndex >= 0) {
                        ui->serialBox->setCurrentIndex(selectedIndex);
                        ui->connectedPhoneList->setCurrentRow(selectedIndex);
                    } else {
                        ui->serialBox->setCurrentIndex(-1);
                        ui->connectedPhoneList->setCurrentRow(-1);
                        ui->connectedPhoneList->clearSelection();
                    }
                }

                const QString finalSerial = selectedIndex >= 0
                    ? ui->serialBox->itemText(selectedIndex).trimmed()
                    : QString();
                handleSelectedSerialChanged(finalSerial);
            } else if (args.contains("show") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ifconfig") && args.contains("wlan0")) {
                QString ip = m_adb.getDeviceIPFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            } else if (args.contains("ip -o a")) {
                QString ip = m_adb.getDeviceIPByIpFromStdOut();
                if (ip.isEmpty()) {
                    log = "ip not find, connect to wifi?";
                    break;
                }
                ui->deviceIpEdt->setEditText(ip);
            }
            break;
        }
        if (!log.isEmpty()) {
            outLog(log, newLine);
        }
    });

    m_hideIcon = new QSystemTrayIcon(this);
    m_hideIcon->setIcon(QIcon(":/image/tray/logo.png"));
    m_menu = new QMenu(this);
    m_quit = new QAction(this);
    m_showWindow = new QAction(this);
    m_restart = new QAction(this);
    m_showWindow->setText(tr("show"));
    QString restartText = tr("restart");
    if (restartText == QLatin1String("restart")) {
        const QString configuredLanguage = Config::getInstance().getLanguage();
        if (configuredLanguage == QLatin1String("zh_CN")) {
            restartText = QStringLiteral("重启");
        } else if (configuredLanguage == QLatin1String("ja_JP")) {
            restartText = QStringLiteral("再起動");
        } else if (configuredLanguage == QLatin1String("ko_KR")) {
            restartText = QStringLiteral("다시 시작");
        } else if (configuredLanguage == QLatin1String("Auto")) {
            switch (QLocale().language()) {
            case QLocale::Chinese:
                restartText = QStringLiteral("重启");
                break;
            case QLocale::Japanese:
                restartText = QStringLiteral("再起動");
                break;
            case QLocale::Korean:
                restartText = QStringLiteral("다시 시작");
                break;
            default:
                break;
            }
        }
    }
    m_restart->setText(restartText);
    m_quit->setText(tr("quit"));
    m_menu->addAction(m_showWindow);
    m_menu->addAction(m_restart);
    m_menu->addAction(m_quit);
    m_hideIcon->setContextMenu(m_menu);
    m_hideIcon->show();
    connect(m_showWindow, &QAction::triggered, this, &Dialog::show);
    connect(m_restart, &QAction::triggered, this, &Dialog::restartApplication);
    connect(m_quit, &QAction::triggered, this, &Dialog::quitApplicationDirectly);
    connect(m_hideIcon, &QSystemTrayIcon::activated, this, &Dialog::slotActivated);

    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceConnected, this, &Dialog::onDeviceConnected);
    connect(&qsc::IDeviceManage::getInstance(), &qsc::IDeviceManage::deviceDisconnected, this, &Dialog::onDeviceDisconnected);
}

Dialog::~Dialog()
{
    qDebug() << "~Dialog()";
    updateBootConfig(false);
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
    delete ui;
}

void Dialog::initUI()
{
    setAttribute(Qt::WA_DeleteOnClose);
    //setWindowFlags(windowFlags() | Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint);

    setWindowTitle(Config::getInstance().getTitle());
#ifdef Q_OS_LINUX
    // Set window icon (inherits from application icon set in main.cpp)
    // If application icon was set, this will use it automatically
    if (!qApp->windowIcon().isNull()) {
        setWindowIcon(qApp->windowIcon());
    }
#endif

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)this->winId(), ThemeManager::getInstance().isDarkTheme());
#endif

    ui->bitRateEdit->setValidator(new QIntValidator(1, 99999, this));

    ui->maxSizeBox->addItem("640");
    ui->maxSizeBox->addItem("720");
    ui->maxSizeBox->addItem("1080");
    ui->maxSizeBox->addItem("1280");
    ui->maxSizeBox->addItem("1920");
    ui->maxSizeBox->addItem(tr("original"));

    ui->formatBox->addItem("mp4");
    ui->formatBox->addItem("mkv");

    ui->lockOrientationBox->addItem(tr("no lock"));
    ui->lockOrientationBox->addItem("0");
    ui->lockOrientationBox->addItem("90");
    ui->lockOrientationBox->addItem("180");
    ui->lockOrientationBox->addItem("270");
    ui->lockOrientationBox->setCurrentIndex(0);

    // 加载IP历史记录
    loadIpHistory();

    // 加载端口历史记录
    loadPortHistory();

    // 为deviceIpEdt添加右键菜单
    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->deviceIpEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showIpEditMenu);
    }
    
    // 为devicePortEdt添加右键菜单
    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(ui->devicePortEdt->lineEdit(), &QWidget::customContextMenuRequested,
                this, &Dialog::showPortEditMenu);
    }

    auto configLayout = qobject_cast<QVBoxLayout *>(ui->configGroupBox->layout());
    if (configLayout) {
        auto *themeWidget = new QWidget(ui->configGroupBox);
        auto *themeLayout = new QHBoxLayout(themeWidget);
        themeLayout->setContentsMargins(0, 0, 0, 0);

        auto *themeLabel = new QLabel(tr("主题模式："), themeWidget);
        m_themeModeBox = new QComboBox(themeWidget);
        m_themeModeBox->addItem(tr("跟随系统"), static_cast<int>(ThemeMode::System));
        m_themeModeBox->addItem(tr("浅色"), static_cast<int>(ThemeMode::Light));
        m_themeModeBox->addItem(tr("深色"), static_cast<int>(ThemeMode::Dark));
        m_themeModeBox->setToolTip(tr("切换整个应用的主题外观，可选择跟随系统、浅色或深色。"));
        themeLabel->setBuddy(m_themeModeBox);
        auto *quitButton = new QPushButton(tr("退出程序"), themeWidget);
        quitButton->setObjectName(QStringLiteral("quitApplicationBtn"));
        quitButton->setAutoDefault(false);
        quitButton->setToolTip(tr("直接退出应用，不隐藏到托盘。"));

        themeLayout->addWidget(themeLabel);
        themeLayout->addWidget(m_themeModeBox);
        themeLayout->addStretch(1);
        themeLayout->addWidget(quitButton);
        configLayout->insertWidget(0, themeWidget);

        connect(quitButton, &QPushButton::clicked, this, &Dialog::quitApplicationDirectly);
    }

    connect(ui->serialBox, &QComboBox::currentTextChanged,
            this, &Dialog::handleSelectedSerialChanged);
    if (m_themeModeBox) {
        connect(m_themeModeBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, &Dialog::onThemeModeChanged);
    }
    connect(&ThemeManager::getInstance(), &ThemeManager::themeChanged, this, [this]() {
#ifdef Q_OS_WIN32
        WinUtils::setDarkBorderToWindow((HWND)this->winId(), ThemeManager::getInstance().isDarkTheme());
#endif
    });

    initGameFeatureUi();
    initUsbMouseConfigUi();
    initControlToolTips();
    handleSelectedSerialChanged(currentSelectedSerial());
    refreshControlToolTips();
}

void Dialog::initGameFeatureUi()
{
    if (m_gameFeatureGroup) {
        return;
    }

    auto rightLayout = qobject_cast<QVBoxLayout *>(ui->rightWidget->layout());
    auto configLayout = qobject_cast<QVBoxLayout *>(ui->configGroupBox->layout());
    if (!rightLayout) {
        return;
    }

    m_gameFeatureGroup = new QGroupBox(tr("游戏功能"), ui->rightWidget);
    m_gameFeatureGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto *groupLayout = new QVBoxLayout(m_gameFeatureGroup);
    groupLayout->setContentsMargins(5, 5, 5, 5);
    groupLayout->setSpacing(4);

    if (configLayout && ui->configWidget4) {
        configLayout->removeWidget(ui->configWidget4);
        ui->configWidget4->setParent(m_gameFeatureGroup);
        groupLayout->addWidget(ui->configWidget4);
    }

    auto *shortcutWidget = new QWidget(m_gameFeatureGroup);
    auto *shortcutLayout = new QHBoxLayout(shortcutWidget);
    shortcutLayout->setContentsMargins(0, 0, 0, 0);

    auto *shortcutLabel = new QLabel(tr("键位编辑热键："), shortcutWidget);
    m_keymapEditorShortcutEdit = new QKeySequenceEdit(shortcutWidget);
    m_keymapEditorShortcutEdit->setObjectName(QStringLiteral("keymapEditorShortcutEdit"));
    m_keymapEditorShortcutEdit->setKeySequence(QKeySequence(QStringLiteral("Ctrl+E")));
    shortcutLabel->setBuddy(m_keymapEditorShortcutEdit);

    shortcutLayout->addWidget(shortcutLabel);
    shortcutLayout->addWidget(m_keymapEditorShortcutEdit, 1);
    groupLayout->addWidget(shortcutWidget);

    m_gameDeviceConfigGroup = new QGroupBox(tr("设备独有配置"), m_gameFeatureGroup);
    m_gameDeviceConfigGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto *deviceGroupLayout = new QVBoxLayout(m_gameDeviceConfigGroup);
    deviceGroupLayout->setContentsMargins(5, 5, 5, 5);
    deviceGroupLayout->setSpacing(4);

    auto *selectedDeviceRow = new QHBoxLayout();
    selectedDeviceRow->setContentsMargins(0, 0, 0, 0);
    selectedDeviceRow->addWidget(new QLabel(tr("当前设备："), m_gameDeviceConfigGroup));
    m_gameSelectedDeviceSerialValue = new QLabel(tr("未选择设备"), m_gameDeviceConfigGroup);
    m_gameSelectedDeviceSerialValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    selectedDeviceRow->addWidget(m_gameSelectedDeviceSerialValue, 1);
    deviceGroupLayout->addLayout(selectedDeviceRow);

    auto *deviceCenterCropRow = new QHBoxLayout();
    deviceCenterCropRow->setContentsMargins(0, 0, 0, 0);
    m_deviceCenterCropCheck = new QCheckBox(tr("center crop"), m_gameDeviceConfigGroup);
    deviceCenterCropRow->addWidget(m_deviceCenterCropCheck);
    deviceCenterCropRow->addStretch(1);

    auto *deviceCenterCropLabel = new QLabel(tr("crop size:"), m_gameDeviceConfigGroup);
    deviceCenterCropRow->addWidget(deviceCenterCropLabel);

    m_deviceCenterCropSizeSpin = new QSpinBox(m_gameDeviceConfigGroup);
    m_deviceCenterCropSizeSpin->setRange(2, 4096);
    m_deviceCenterCropSizeSpin->setSingleStep(2);
    m_deviceCenterCropSizeSpin->setValue(256);
    m_deviceCenterCropSizeSpin->setEnabled(false);
    deviceCenterCropLabel->setBuddy(m_deviceCenterCropSizeSpin);
    deviceCenterCropRow->addWidget(m_deviceCenterCropSizeSpin);
    deviceGroupLayout->addLayout(deviceCenterCropRow);

    groupLayout->addWidget(m_gameDeviceConfigGroup);

    connect(m_keymapEditorShortcutEdit, &QKeySequenceEdit::keySequenceChanged,
            this, &Dialog::on_keymapEditorShortcutEdit_keySequenceChanged);
    connect(m_deviceCenterCropCheck, &QCheckBox::toggled,
            this, &Dialog::onSelectedDeviceCenterCropConfigEdited);
    connect(m_deviceCenterCropSizeSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Dialog::onSelectedDeviceCenterCropConfigEdited);

    rightLayout->insertWidget(1, m_gameFeatureGroup);
}

void Dialog::initUsbMouseConfigUi()
{
    if (m_mouseConfigToggleBtn || m_mouseConfigContent) {
        return;
    }

    auto usbLayout = qobject_cast<QVBoxLayout *>(ui->usbGroupBox->layout());
    if (!usbLayout) {
        return;
    }

    m_mouseConfigToggleBtn = new QToolButton(ui->usbGroupBox);
    m_mouseConfigToggleBtn->setText(tr("鼠标显示设置"));
    m_mouseConfigToggleBtn->setCheckable(true);
    m_mouseConfigToggleBtn->setChecked(false);
    m_mouseConfigToggleBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_mouseConfigToggleBtn->setArrowType(Qt::RightArrow);
    connect(m_mouseConfigToggleBtn, &QToolButton::toggled, this, &Dialog::setMouseConfigExpanded);
    usbLayout->addWidget(m_mouseConfigToggleBtn);

    m_mouseConfigContent = new QWidget(ui->usbGroupBox);
    auto mouseLayout = new QFormLayout(m_mouseConfigContent);
    mouseLayout->setContentsMargins(18, 0, 0, 0);
    mouseLayout->setSpacing(4);

    const QString mouseConfigScopeTip = tr("仅对当前选中设备生效。");
    const QString remoteCursorToolTip = tr("控制手机画面里的远端黑色鼠标光标是否渲染；关闭后会按现有隐藏逻辑清掉。\n%1")
        .arg(mouseConfigScopeTip);
    const QString cursorSizeToolTip = tr("远端光标大小，范围 8~128。\n%1").arg(mouseConfigScopeTip);
    const QString normalMouseCompatToolTip = tr("非相对视角下的普通鼠标兼容总开关；关闭时下面几项兼容参数不生效。\n%1")
        .arg(mouseConfigScopeTip);
    const QString touchPriorityToolTip = tr("让普通点击和滑动触控优先于远端黑光标刷新。\n%1")
        .arg(mouseConfigScopeTip);
    const QString cursorThrottleToolTip = tr("启用普通模式远端黑光标限频与点击静默；关闭后恢复更即时的光标发送。\n%1")
        .arg(mouseConfigScopeTip);
    const QString flushIntervalToolTip = tr("黑光标刷新间隔，范围 16~100 ms；越大越稳，但越不跟手。\n%1")
        .arg(mouseConfigScopeTip);
    const QString clickSuppressionToolTip = tr("点击前后黑光标静默时间，范围 0~300 ms；越大越优先保证点击。\n%1")
        .arg(mouseConfigScopeTip);
    const QString tapMinHoldToolTip = tr("普通左键轻点最小按压时长，范围 0~40 ms；0 表示关闭，越大越稳但点击感更慢。\n%1")
        .arg(mouseConfigScopeTip);

    auto addLabeledMouseConfigRow = [this](QFormLayout *layout, const QString &labelText,
                                           const QString &toolTip, QWidget *field) {
        auto *label = new QLabel(labelText, m_mouseConfigContent);
        label->setToolTip(toolTip);
        field->setToolTip(toolTip);
        layout->addRow(label, field);
    };

    auto *selectedDeviceLabel = new QLabel(tr("当前设备："), m_mouseConfigContent);
    selectedDeviceLabel->setToolTip(mouseConfigScopeTip);
    m_selectedDeviceSerialValue = new QLabel(tr("未选择设备"), m_mouseConfigContent);
    m_selectedDeviceSerialValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_selectedDeviceSerialValue->setToolTip(mouseConfigScopeTip);
    mouseLayout->addRow(selectedDeviceLabel, m_selectedDeviceSerialValue);

    m_renderRemoteCursorCheck = new QCheckBox(m_mouseConfigContent);
    addLabeledMouseConfigRow(mouseLayout, tr("Render Remote Cursor"), remoteCursorToolTip, m_renderRemoteCursorCheck);

    m_cursorSizeSpin = new QSpinBox(m_mouseConfigContent);
    m_cursorSizeSpin->setRange(8, 128);
    addLabeledMouseConfigRow(mouseLayout, tr("Cursor Size Px"), cursorSizeToolTip, m_cursorSizeSpin);

    m_normalMouseCompatEnabledCheck = new QCheckBox(m_mouseConfigContent);
    addLabeledMouseConfigRow(mouseLayout, tr("Normal Mouse Compat"), normalMouseCompatToolTip, m_normalMouseCompatEnabledCheck);

    m_normalMouseTouchPriorityEnabledCheck = new QCheckBox(m_mouseConfigContent);
    addLabeledMouseConfigRow(mouseLayout, tr("Touch Priority"), touchPriorityToolTip, m_normalMouseTouchPriorityEnabledCheck);

    m_normalMouseCursorThrottleEnabledCheck = new QCheckBox(m_mouseConfigContent);
    addLabeledMouseConfigRow(mouseLayout, tr("Cursor Throttle"), cursorThrottleToolTip, m_normalMouseCursorThrottleEnabledCheck);

    m_normalMouseCursorFlushIntervalSpin = new QSpinBox(m_mouseConfigContent);
    m_normalMouseCursorFlushIntervalSpin->setRange(16, 100);
    m_normalMouseCursorFlushIntervalSpin->setSuffix(tr(" ms"));
    addLabeledMouseConfigRow(mouseLayout, tr("Cursor Flush Interval"), flushIntervalToolTip, m_normalMouseCursorFlushIntervalSpin);

    m_normalMouseCursorClickSuppressionSpin = new QSpinBox(m_mouseConfigContent);
    m_normalMouseCursorClickSuppressionSpin->setRange(0, 300);
    m_normalMouseCursorClickSuppressionSpin->setSuffix(tr(" ms"));
    addLabeledMouseConfigRow(mouseLayout, tr("Click Suppression"), clickSuppressionToolTip, m_normalMouseCursorClickSuppressionSpin);

    m_normalMouseTapMinHoldSpin = new QSpinBox(m_mouseConfigContent);
    m_normalMouseTapMinHoldSpin->setRange(0, 40);
    m_normalMouseTapMinHoldSpin->setSuffix(tr(" ms"));
    addLabeledMouseConfigRow(mouseLayout, tr("Tap Min Hold"), tapMinHoldToolTip, m_normalMouseTapMinHoldSpin);

    m_mouseConfigContent->setToolTip(mouseConfigScopeTip);
    usbLayout->addWidget(m_mouseConfigContent);
    setMouseConfigExpanded(false);

    connect(m_renderRemoteCursorCheck, &QCheckBox::toggled, this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseCompatEnabledCheck, &QCheckBox::toggled, this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseTouchPriorityEnabledCheck, &QCheckBox::toggled, this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseCursorThrottleEnabledCheck, &QCheckBox::toggled, this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_cursorSizeSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseCursorFlushIntervalSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseCursorClickSuppressionSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Dialog::onSelectedDeviceMouseConfigEdited);
    connect(m_normalMouseTapMinHoldSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this, &Dialog::onSelectedDeviceMouseConfigEdited);
}

void Dialog::initControlToolTips()
{
    const QString themeToolTip = tr("切换整个应用的主题外观，可选择跟随系统、浅色或深色。");
    const QString bitRateValueToolTip = tr("设置视频码率数值。最终码率会和右侧单位组合计算；数值越大画质越高，但带宽和性能占用也越大。");
    const QString bitRateUnitToolTip = tr("选择码率单位。最终码率 = 左侧数值 x 当前单位。");
    const QString maxSizeToolTip = tr("限制视频长边分辨率。值越小越省带宽；original 表示保持设备原始尺寸。");
    const QString maxFpsToolTip = tr("设置全局最大帧率。0 表示不限制；修改后对新启动或重启后的服务生效。");
    const QString formatToolTip = tr("选择录屏文件格式，仅在开启录屏时生效。");
    const QString orientationToolTip = tr("锁定采集方向。选择具体角度后会强制按该方向采集画面。");
    const QString localTextInputToolTip = tr("启用视频窗口里的本地文本输入浮层。开启后可用右侧快捷键呼出输入框，一次性把文字发送到设备。");
    const QString gameScriptToolTip = tr("选择要绑定到当前设备的视频窗口脚本。点击“应用脚本”后会立即下发到已连接设备。");
    const QString recordScreenToolTip = tr("连接设备时自动开始录制。只影响新启动的会话；运行中的开始和停止请使用视频窗口旁的录制按钮。");
    const QString deviceCenterCropToolTip = tr("为当前设备启用独有的中心裁切参数。开启后只对当前选中设备生效。");
    const QString deviceCenterCropSizeToolTip = tr("设置当前设备中心裁切尺寸，只有开启中心裁切后才会生效。");

    ui->useSingleModeCheck->setToolTip(tr("切换为快捷连接模式。开启后会隐藏右侧高级配置，只保留左侧快速连接入口。"));
    ui->wifiConnectBtn->setToolTip(tr("按预设流程尝试无线连接：刷新设备、读取 IP、切换 adbd 到 tcpip、执行 adb connect，然后启动投屏。设备需要先通过 USB 被 adb 识别。"));
    ui->usbConnectBtn->setToolTip(tr("按预设流程尝试 USB 直连：停止现有会话、刷新设备列表，并连接第一个 USB 设备。"));
    ui->connectedPhoneList->setToolTip(tr("显示当前 adb 识别到的设备。双击某一项会选中该设备并直接启动投屏服务。"));
    ui->adbCommandEdt->setToolTip(tr("输入要执行的 adb 子命令，不需要包含 adb 本体。执行结果会追加到下方日志。"));
    ui->adbCommandBtn->setToolTip(tr("执行上方 adb 命令。命令运行期间，新的 adb 操作会被阻塞。"));
    ui->stopAdbBtn->setToolTip(tr("强制停止当前正在运行的 adb 命令。"));
    ui->clearOut->setToolTip(tr("清空下方日志输出区域。"));
    ui->bitRateEdit->setToolTip(bitRateValueToolTip);
    ui->bitRateBox->setToolTip(bitRateUnitToolTip);
    ui->maxSizeBox->setToolTip(maxSizeToolTip);
    ui->maxFpsSpin->setToolTip(maxFpsToolTip);
    ui->formatBox->setToolTip(formatToolTip);
    ui->lockOrientationBox->setToolTip(orientationToolTip);
    ui->selectRecordPathBtn->setToolTip(tr("选择录屏保存目录。未设置目录时无法开启录屏。"));
    ui->localTextInputCheck->setToolTip(localTextInputToolTip);
    ui->openAppFolderBtn->setToolTip(tr("打开当前 QtScrcpy 可执行文件所在目录。"));
    ui->gameBox->setToolTip(gameScriptToolTip);
    ui->refreshGameScriptBtn->setToolTip(tr("重新扫描 keymap 目录里的脚本文件并刷新列表。"));
    ui->applyScriptBtn->setToolTip(tr("将当前选中的脚本立即应用到已连接设备和已打开的视频窗口。"));
    ui->fpsCheck->setToolTip(tr("在视频窗口显示实时帧率信息。只影响窗口显示，不改变采集帧率。"));
    ui->notDisplayCheck->setToolTip(tr("连接后不显示设备画面，只保留后台会话。通常配合录屏使用；未设置录屏目录时不会允许勾选。"));
    ui->alwaysTopCheck->setToolTip(tr("让视频窗口保持置顶。只影响新打开或当前已打开的视频窗口。"));
    ui->recordScreenCheck->setToolTip(recordScreenToolTip);
    ui->useReverseCheck->setToolTip(tr("使用 reverse 方式建立设备与电脑之间的投屏通道。某些环境更稳，但依赖设备端 adb 能力。"));
    ui->closeScreenCheck->setToolTip(tr("连接成功后尝试关闭设备屏幕显示。只会熄屏，不会断开会话。"));
    ui->framelessCheck->setToolTip(tr("让视频窗口使用无边框样式显示。"));
    ui->stayAwakeCheck->setToolTip(tr("连接期间请求设备保持唤醒，减少自动息屏导致的操作中断。"));
    ui->showToolbar->setToolTip(tr("在视频窗口旁显示悬浮工具栏。关闭后仍可通过主窗口继续操作设备。"));
    ui->userNameEdt->setToolTip(tr("给当前选中的设备设置本地备注名，只保存在本机配置里。"));
    ui->updateNameBtn->setToolTip(tr("保存上方备注名并刷新设备列表显示。留空时会恢复默认名称 Phone。"));
    ui->serialBox->setToolTip(tr("选择要操作或连接的设备序列号。右侧的大部分设备操作都会作用到当前选中设备。"));
    ui->startServerBtn->setToolTip(tr("按当前配置启动所选设备的投屏服务，并打开视频窗口。"));
    ui->stopServerBtn->setToolTip(tr("断开当前选中设备的投屏会话，并关闭对应视频窗口。"));
    ui->restartServerBtn->setToolTip(tr("重启当前选中设备的投屏服务。会先断开，再按当前配置重新连接。"));
    ui->stopAllServerBtn->setToolTip(tr("断开所有已连接设备的投屏会话。"));
    ui->updateDevice->setToolTip(tr("执行 adb devices 刷新设备列表。"));
    ui->getIPBtn->setToolTip(tr("从当前 USB 连接设备读取 WLAN IP 地址，并填入无线连接区域。设备未连入 Wi-Fi 时可能取不到。"));
    ui->startAdbdBtn->setToolTip(tr("在当前设备上执行 adb tcpip 5555，把 adbd 切到无线调试端口。通常需要设备先通过 USB 连接。"));
    ui->installSndcpyBtn->setToolTip(tr("仅向当前设备安装或准备 sndcpy 音频组件，不会开始播放。"));
    ui->startAudioBtn->setToolTip(tr("启动当前设备的音频转发播放。需要设备已连接，且 sndcpy 组件可用。"));
    ui->stopAudioBtn->setToolTip(tr("停止当前音频转发播放。"));
    ui->wirelessConnectBtn->setToolTip(tr("对上方 IP 和端口执行 adb connect。成功后会记录历史；不会自动启动投屏，除非使用左侧快捷连接流程。"));
    ui->wirelessDisConnectBtn->setToolTip(tr("对上方 IP 执行 adb disconnect，断开无线 adb 连接。"));

    if (m_themeModeBox) {
        m_themeModeBox->setToolTip(themeToolTip);
    }

    if (m_deviceCenterCropCheck) {
        m_deviceCenterCropCheck->setToolTip(deviceCenterCropToolTip);
    }
    if (m_deviceCenterCropSizeSpin) {
        m_deviceCenterCropSizeSpin->setToolTip(deviceCenterCropSizeToolTip);
    }
    if (m_mouseConfigToggleBtn) {
        m_mouseConfigToggleBtn->setToolTip(buildMouseConfigToggleToolTip());
    }

    refreshAutoUpdateToolTips();
}

void Dialog::refreshControlToolTips()
{
    ui->recordPathEdt->setToolTip(buildRecordPathToolTip());
    ui->localTextInputShortcutEdit->setToolTip(buildLocalTextInputShortcutToolTip());
    if (m_keymapEditorShortcutEdit) {
        m_keymapEditorShortcutEdit->setToolTip(buildKeymapEditorShortcutToolTip());
    }
    setComboAndLineEditToolTip(ui->deviceIpEdt, buildDeviceIpToolTip());
    setComboAndLineEditToolTip(ui->devicePortEdt, buildDevicePortToolTip());
    refreshAutoUpdateToolTips();

    if (m_mouseConfigToggleBtn) {
        m_mouseConfigToggleBtn->setToolTip(buildMouseConfigToggleToolTip());
    }
}

int Dialog::currentAutoUpdateIntervalSec() const
{
    return qBound(1, ui->autoUpdateIntervalSpin->value(), 3600);
}

int Dialog::currentAutoUpdateIntervalMs() const
{
    return currentAutoUpdateIntervalSec() * 1000;
}

void Dialog::applyAutoUpdateTimerState()
{
    const bool enabled = ui->autoUpdatecheckBox->isChecked();
    ui->autoUpdateIntervalSpin->setEnabled(enabled);

    if (enabled) {
        m_autoUpdatetimer.start(currentAutoUpdateIntervalMs());
    } else {
        m_autoUpdatetimer.stop();
    }
}

void Dialog::refreshAutoUpdateToolTips()
{
    const int intervalSec = currentAutoUpdateIntervalSec();
    ui->autoUpdatecheckBox->setToolTip(
        tr("每 %1 秒自动执行一次 adb devices 刷新设备列表。开启后会持续占用一轮 adb 轮询。").arg(intervalSec));
    ui->autoUpdateIntervalSpin->setToolTip(
        tr("控制左侧设备列表自动刷新的时间间隔，单位为秒。当前值：%1 秒；值越小轮询越频繁。").arg(intervalSec));
}

QString Dialog::buildRecordPathToolTip() const
{
    const QString path = ui->recordPathEdt->text().trimmed();
    QString toolTip = tr("设置录屏保存目录。连接时自动录制和视频窗口工具栏里的开始录制都会复用这里的路径。");
    toolTip += path.isEmpty()
        ? tr("\n当前路径：未设置。")
        : tr("\n当前路径：%1").arg(path);
    return toolTip;
}

QString Dialog::buildLocalTextInputShortcutToolTip() const
{
    const QString shortcut = ui->localTextInputShortcutEdit->keySequence().toString(QKeySequence::NativeText).trimmed();
    QString toolTip = tr("设置视频窗口里呼出本地文本输入浮层的快捷键。只有启用本地文本输入后才会生效。");
    toolTip += shortcut.isEmpty()
        ? tr("\n当前快捷键：未设置。")
        : tr("\n当前快捷键：%1。").arg(shortcut);
    if (!ui->localTextInputCheck->isChecked()) {
        toolTip += tr("\n当前状态：本地文本输入未启用。");
    }
    return toolTip;
}

QString Dialog::buildKeymapEditorShortcutToolTip() const
{
    const QString shortcut = currentKeymapEditorShortcut().toString(QKeySequence::NativeText).trimmed();
    QString toolTip = tr("设置视频窗口里打开或关闭键位编辑器的快捷键。编辑器录键状态下也会复用这个快捷键来取消并关闭。");
    toolTip += shortcut.isEmpty()
        ? tr("\n当前快捷键：未设置。")
        : tr("\n当前快捷键：%1。").arg(shortcut);
    if (shortcut.isEmpty()) {
        toolTip += tr("\n当前状态：键盘快捷入口已禁用。");
    }
    return toolTip;
}

QString Dialog::buildDeviceIpToolTip() const
{
    return tr("输入或选择目标设备的 IP 地址。成功连接后会自动记录历史；右键输入框可清空历史记录。");
}

QString Dialog::buildDevicePortToolTip() const
{
    return tr("输入或选择目标设备端口。留空时会使用默认端口 5555；成功连接后会自动记录历史，右键输入框可清空历史记录。");
}

QString Dialog::buildMouseConfigToggleToolTip() const
{
    const bool expanded = m_mouseConfigToggleBtn && m_mouseConfigToggleBtn->isChecked();
    return expanded
        ? tr("展开区域里的参数仅对当前选中设备生效。\n当前状态：已展开，点击后收起。")
        : tr("展开当前选中设备的鼠标显示兼容参数。\n当前状态：已收起。");
}

QKeySequence Dialog::currentKeymapEditorShortcut() const
{
    if (!m_keymapEditorShortcutEdit) {
        return QKeySequence(QStringLiteral("Ctrl+E"));
    }
    return normalizeSingleShortcut(m_keymapEditorShortcutEdit->keySequence());
}

QString Dialog::currentSelectedSerial() const
{
    if (!ui || !ui->serialBox) {
        return QString();
    }
    return ui->serialBox->currentText().trimmed();
}

ThemeMode Dialog::currentThemeModeSelection() const
{
    if (!m_themeModeBox) {
        return ThemeMode::System;
    }

    const QVariant value = m_themeModeBox->currentData();
    if (!value.isValid()) {
        return ThemeMode::System;
    }

    return static_cast<ThemeMode>(value.toInt());
}

QString Dialog::formatSelectedDeviceDisplayName(const QString &serial) const
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        return tr("未选择设备");
    }

    const QString nickName = Config::getInstance().getNickName(trimmedSerial).trimmed();
    if (nickName.isEmpty() || nickName == QLatin1String("Phone")) {
        return trimmedSerial;
    }

    return QStringLiteral("%1 - %2").arg(nickName, trimmedSerial);
}

void Dialog::setMouseConfigExpanded(bool expanded)
{
    if (!m_mouseConfigToggleBtn || !m_mouseConfigContent) {
        return;
    }

    m_mouseConfigToggleBtn->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    m_mouseConfigContent->setVisible(expanded);
    refreshControlToolTips();
}

void Dialog::updateSelectedDeviceConfigControlState()
{
    if (!m_gameDeviceConfigGroup && !m_mouseConfigContent) {
        return;
    }

    const bool hasSerial = !currentSelectedSerial().isEmpty();
    const bool centerCropEnabled = hasSerial
        && m_deviceCenterCropCheck
        && m_deviceCenterCropCheck->isChecked();
    const bool compatEnabled = hasSerial
        && m_normalMouseCompatEnabledCheck
        && m_normalMouseCompatEnabledCheck->isChecked();

    if (m_gameDeviceConfigGroup) {
        m_gameDeviceConfigGroup->setEnabled(hasSerial);
    }
    if (m_deviceCenterCropCheck) {
        m_deviceCenterCropCheck->setEnabled(hasSerial);
    }
    if (m_deviceCenterCropSizeSpin) {
        m_deviceCenterCropSizeSpin->setEnabled(centerCropEnabled);
    }
    if (m_mouseConfigContent) {
        m_mouseConfigContent->setEnabled(hasSerial);
    }
    if (m_renderRemoteCursorCheck) {
        m_renderRemoteCursorCheck->setEnabled(hasSerial);
    }
    if (m_cursorSizeSpin) {
        m_cursorSizeSpin->setEnabled(hasSerial);
    }
    if (m_normalMouseCompatEnabledCheck) {
        m_normalMouseCompatEnabledCheck->setEnabled(hasSerial);
    }
    if (m_normalMouseTouchPriorityEnabledCheck) {
        m_normalMouseTouchPriorityEnabledCheck->setEnabled(compatEnabled);
    }
    if (m_normalMouseCursorThrottleEnabledCheck) {
        m_normalMouseCursorThrottleEnabledCheck->setEnabled(compatEnabled);
    }
    if (m_normalMouseCursorFlushIntervalSpin) {
        m_normalMouseCursorFlushIntervalSpin->setEnabled(compatEnabled);
    }
    if (m_normalMouseCursorClickSuppressionSpin) {
        m_normalMouseCursorClickSuppressionSpin->setEnabled(compatEnabled);
    }
    if (m_normalMouseTapMinHoldSpin) {
        m_normalMouseTapMinHoldSpin->setEnabled(compatEnabled);
    }
}

void Dialog::updateSelectedDeviceConfigUi(const QString &serial)
{
    if (!m_gameDeviceConfigGroup && !m_mouseConfigContent) {
        return;
    }

    const QString trimmedSerial = serial.trimmed();
    m_updatingSelectedDeviceConfigUi = true;

    if (m_selectedDeviceSerialValue) {
        m_selectedDeviceSerialValue->setText(formatSelectedDeviceDisplayName(trimmedSerial));
    }
    if (m_gameSelectedDeviceSerialValue) {
        m_gameSelectedDeviceSerialValue->setText(formatSelectedDeviceDisplayName(trimmedSerial));
    }

    const bool centerCropEnabled = Config::getInstance().isDeviceCenterCropEnabled(trimmedSerial);
    const int centerCropSize = centerCropEnabled
        ? Config::getInstance().getDeviceCenterCropSize(trimmedSerial)
        : 256;
    DeviceMouseConfig config = Config::getInstance().getDeviceMouseConfig(trimmedSerial);

    const QSignalBlocker centerCropCheckBlocker(m_deviceCenterCropCheck);
    const QSignalBlocker centerCropSizeBlocker(m_deviceCenterCropSizeSpin);
    const QSignalBlocker remoteCursorBlocker(m_renderRemoteCursorCheck);
    const QSignalBlocker cursorSizeBlocker(m_cursorSizeSpin);
    const QSignalBlocker compatBlocker(m_normalMouseCompatEnabledCheck);
    const QSignalBlocker touchPriorityBlocker(m_normalMouseTouchPriorityEnabledCheck);
    const QSignalBlocker throttleBlocker(m_normalMouseCursorThrottleEnabledCheck);
    const QSignalBlocker flushBlocker(m_normalMouseCursorFlushIntervalSpin);
    const QSignalBlocker suppressionBlocker(m_normalMouseCursorClickSuppressionSpin);
    const QSignalBlocker tapHoldBlocker(m_normalMouseTapMinHoldSpin);

    if (m_deviceCenterCropCheck) {
        m_deviceCenterCropCheck->setChecked(centerCropEnabled);
    }
    if (m_deviceCenterCropSizeSpin) {
        m_deviceCenterCropSizeSpin->setValue(centerCropSize);
    }
    if (m_renderRemoteCursorCheck) {
        m_renderRemoteCursorCheck->setChecked(config.remoteCursorEnabled);
    }
    if (m_cursorSizeSpin) {
        m_cursorSizeSpin->setValue(config.cursorSizePx);
    }
    if (m_normalMouseCompatEnabledCheck) {
        m_normalMouseCompatEnabledCheck->setChecked(config.normalMouseCompatEnabled);
    }
    if (m_normalMouseTouchPriorityEnabledCheck) {
        m_normalMouseTouchPriorityEnabledCheck->setChecked(config.normalMouseTouchPriorityEnabled);
    }
    if (m_normalMouseCursorThrottleEnabledCheck) {
        m_normalMouseCursorThrottleEnabledCheck->setChecked(config.normalMouseCursorThrottleEnabled);
    }
    if (m_normalMouseCursorFlushIntervalSpin) {
        m_normalMouseCursorFlushIntervalSpin->setValue(config.normalMouseCursorFlushIntervalMs);
    }
    if (m_normalMouseCursorClickSuppressionSpin) {
        m_normalMouseCursorClickSuppressionSpin->setValue(config.normalMouseCursorClickSuppressionMs);
    }
    if (m_normalMouseTapMinHoldSpin) {
        m_normalMouseTapMinHoldSpin->setValue(config.normalMouseTapMinHoldMs);
    }

    updateSelectedDeviceConfigControlState();
    m_updatingSelectedDeviceConfigUi = false;
}

void Dialog::saveSelectedDeviceMouseConfig()
{
    if (m_updatingSelectedDeviceConfigUi) {
        return;
    }

    const QString serial = currentSelectedSerial();
    if (serial.isEmpty()) {
        updateSelectedDeviceConfigControlState();
        return;
    }

    DeviceMouseConfig config;
    config.remoteCursorEnabled = m_renderRemoteCursorCheck && m_renderRemoteCursorCheck->isChecked();
    config.cursorSizePx = m_cursorSizeSpin ? m_cursorSizeSpin->value() : 24;
    config.normalMouseCompatEnabled = m_normalMouseCompatEnabledCheck && m_normalMouseCompatEnabledCheck->isChecked();
    config.normalMouseTouchPriorityEnabled = m_normalMouseTouchPriorityEnabledCheck
        && m_normalMouseTouchPriorityEnabledCheck->isChecked();
    config.normalMouseCursorThrottleEnabled = m_normalMouseCursorThrottleEnabledCheck
        && m_normalMouseCursorThrottleEnabledCheck->isChecked();
    config.normalMouseCursorFlushIntervalMs = m_normalMouseCursorFlushIntervalSpin
        ? m_normalMouseCursorFlushIntervalSpin->value() : 33;
    config.normalMouseCursorClickSuppressionMs = m_normalMouseCursorClickSuppressionSpin
        ? m_normalMouseCursorClickSuppressionSpin->value() : 120;
    config.normalMouseTapMinHoldMs = m_normalMouseTapMinHoldSpin
        ? m_normalMouseTapMinHoldSpin->value() : 16;

    Config::getInstance().setDeviceMouseConfig(serial, config);
    updateSelectedDeviceConfigControlState();
}

void Dialog::onSelectedDeviceMouseConfigEdited()
{
    saveSelectedDeviceMouseConfig();
}

void Dialog::saveSelectedDeviceCenterCropConfig()
{
    if (m_updatingSelectedDeviceConfigUi) {
        return;
    }

    const QString serial = currentSelectedSerial();
    if (serial.isEmpty()) {
        updateSelectedDeviceConfigControlState();
        return;
    }

    const bool enabled = m_deviceCenterCropCheck && m_deviceCenterCropCheck->isChecked();
    if (enabled) {
        const int cropSize = m_deviceCenterCropSizeSpin ? m_deviceCenterCropSizeSpin->value() : 256;
        Config::getInstance().setDeviceCenterCropSize(serial, cropSize);
    } else {
        Config::getInstance().clearDeviceCenterCropSize(serial);
        if (m_deviceCenterCropSizeSpin) {
            const QSignalBlocker blocker(m_deviceCenterCropSizeSpin);
            m_deviceCenterCropSizeSpin->setValue(256);
        }
    }

    updateSelectedDeviceConfigControlState();
}

void Dialog::onSelectedDeviceCenterCropConfigEdited()
{
    saveSelectedDeviceCenterCropConfig();
}

void Dialog::updateBootConfig(bool toView)
{
    if (toView) {
        UserBootConfig config = Config::getInstance().getUserBootConfig();

        if (config.bitRate == 0) {
            ui->bitRateBox->setCurrentText("Mbps");
        } else if (config.bitRate % 1000000 == 0) {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000000));
            ui->bitRateBox->setCurrentText("Mbps");
        } else {
            ui->bitRateEdit->setText(QString::number(config.bitRate / 1000));
            ui->bitRateBox->setCurrentText("Kbps");
        }

        if (m_themeModeBox) {
            const QSignalBlocker blocker(m_themeModeBox);
            const int index = m_themeModeBox->findData(static_cast<int>(config.themeMode));
            m_themeModeBox->setCurrentIndex(index >= 0 ? index : 0);
        }
        const QSignalBlocker autoUpdateBlocker(ui->autoUpdatecheckBox);
        const QSignalBlocker autoUpdateIntervalBlocker(ui->autoUpdateIntervalSpin);
        const QSignalBlocker keymapEditorShortcutBlocker(m_keymapEditorShortcutEdit);
        ui->maxFpsSpin->setValue(config.maxFps);
        ui->maxSizeBox->setCurrentIndex(config.maxSizeIndex);
        ui->formatBox->setCurrentIndex(config.recordFormatIndex);
        ui->recordPathEdt->setText(config.recordPath);
        ui->lockOrientationBox->setCurrentIndex(config.lockOrientationIndex);
        ui->localTextInputCheck->setChecked(config.localTextInputEnabled);
        ui->localTextInputShortcutEdit->setKeySequence(QKeySequence::fromString(config.localTextInputShortcut, QKeySequence::PortableText));
        if (m_keymapEditorShortcutEdit) {
            m_keymapEditorShortcutEdit->setKeySequence(normalizeSingleShortcut(
                QKeySequence::fromString(config.keymapEditorShortcut, QKeySequence::PortableText)));
        }
        ui->framelessCheck->setChecked(config.framelessWindow);
        ui->recordScreenCheck->setChecked(config.recordScreen);
        ui->notDisplayCheck->setChecked(config.recordBackground);
        ui->useReverseCheck->setChecked(config.reverseConnect);
        ui->fpsCheck->setChecked(config.showFPS);
        ui->alwaysTopCheck->setChecked(config.windowOnTop);
        ui->closeScreenCheck->setChecked(config.autoOffScreen);
        ui->stayAwakeCheck->setChecked(config.keepAlive);
        ui->useSingleModeCheck->setChecked(config.simpleMode);
        ui->autoUpdatecheckBox->setChecked(config.autoUpdateDevice);
        ui->autoUpdateIntervalSpin->setValue(config.autoUpdateIntervalSec);
        ui->showToolbar->setChecked(config.showToolbar);
        applyAutoUpdateTimerState();
        refreshControlToolTips();
        applyLocalTextInputConfigToOpenVideoForms();
        applyKeymapEditorShortcutToOpenVideoForms();
    } else {
        UserBootConfig config;

        config.bitRate = getBitRate();
        config.themeMode = currentThemeModeSelection();
        config.maxFps = ui->maxFpsSpin->value();
        config.maxSizeIndex = ui->maxSizeBox->currentIndex();
        config.recordFormatIndex = ui->formatBox->currentIndex();
        config.recordPath = ui->recordPathEdt->text();
        config.lockOrientationIndex = ui->lockOrientationBox->currentIndex();
        config.localTextInputEnabled = ui->localTextInputCheck->isChecked();
        config.localTextInputShortcut = ui->localTextInputShortcutEdit->keySequence().toString(QKeySequence::PortableText);
        config.keymapEditorShortcut = currentKeymapEditorShortcut().toString(QKeySequence::PortableText);
        config.recordScreen = ui->recordScreenCheck->isChecked();
        config.recordBackground = ui->notDisplayCheck->isChecked();
        config.reverseConnect = ui->useReverseCheck->isChecked();
        config.showFPS = ui->fpsCheck->isChecked();
        config.windowOnTop = ui->alwaysTopCheck->isChecked();
        config.autoOffScreen = ui->closeScreenCheck->isChecked();
        config.framelessWindow = ui->framelessCheck->isChecked();
        config.keepAlive = ui->stayAwakeCheck->isChecked();
        config.simpleMode = ui->useSingleModeCheck->isChecked();
        config.autoUpdateDevice = ui->autoUpdatecheckBox->isChecked();
        config.autoUpdateIntervalSec = currentAutoUpdateIntervalSec();
        config.showToolbar = ui->showToolbar->isChecked();

        // 保存当前IP到历史记录
        QString currentIp = ui->deviceIpEdt->currentText().trimmed();
        if (!currentIp.isEmpty()) {
            saveIpHistory(currentIp);
        }

        Config::getInstance().setUserBootConfig(config);
    }
}

void Dialog::onThemeModeChanged(int index)
{
    Q_UNUSED(index);
    UserBootConfig config = Config::getInstance().getUserBootConfig();
    config.themeMode = currentThemeModeSelection();
    Config::getInstance().setUserBootConfig(config);
    ThemeManager::getInstance().applyConfiguredTheme();
}

void Dialog::execAdbCmd()
{
    if (checkAdbRun()) {
        return;
    }
    QString cmd = ui->adbCommandEdt->text().trimmed();
    outLog("adb " + cmd, false);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", Qt::SkipEmptyParts));
#else
    m_adb.execute(ui->serialBox->currentText().trimmed(), cmd.split(" ", QString::SkipEmptyParts));
#endif
}

void Dialog::delayMs(int ms)
{
    QTime dieTime = QTime::currentTime().addMSecs(ms);

    while (QTime::currentTime() < dieTime) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
}

QString Dialog::getGameScript(const QString &fileName)
{
    if (fileName.isEmpty()) {
        return "";
    }

    QFile loadFile(getGameScriptPath(fileName));
    if (!loadFile.open(QIODevice::ReadOnly)) {
        outLog("open file failed:" + fileName, true);
        return "";
    }

    QString ret = loadFile.readAll();
    loadFile.close();
    return ret;
}

QString Dialog::getGameScriptPath(const QString &fileName) const
{
    if (fileName.isEmpty()) {
        return QString();
    }
    return getKeyMapPath() + "/" + fileName;
}

void Dialog::slotActivated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
    case QSystemTrayIcon::Trigger:
#ifdef Q_OS_WIN32
        this->show();
#endif
        break;
    default:
        break;
    }
}

void Dialog::closeEvent(QCloseEvent *event)
{
    this->hide();
    if (!Config::getInstance().getTrayMessageShown()) {
        Config::getInstance().setTrayMessageShown(true);
        m_hideIcon->showMessage(tr("Notice"),
                                tr("Hidden here!"),
                                QSystemTrayIcon::Information,
                                3000);
    }
    event->ignore();
}

void Dialog::on_updateDevice_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("update devices...", false);
    m_adb.execute("", QStringList() << "devices");
}

void Dialog::on_startServerBtn_clicked()
{
    updateBootConfig(false);
    outLog("start server...", false);
    qsc::IDeviceManage::getInstance().connectDevice(buildDeviceParams(ui->serialBox->currentText().trimmed()));
}

void Dialog::on_stopServerBtn_clicked()
{
    if (qsc::IDeviceManage::getInstance().disconnectDevice(ui->serialBox->currentText().trimmed())) {
        outLog("stop server");
    }
}

void Dialog::on_restartServerBtn_clicked()
{
    onRestartDeviceRequested(currentSelectedSerial());
}

void Dialog::on_wirelessConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    if (addr.isEmpty()) {
        outLog("error: device ip is null", false);
        return;
    }

    if (!ui->devicePortEdt->currentText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->currentText().trimmed();
    } else if (!ui->devicePortEdt->lineEdit()->placeholderText().isEmpty()) {
        addr += ":";
        addr += ui->devicePortEdt->lineEdit()->placeholderText().trimmed();
    } else {
        outLog("error: device port is null", false);
        return;
    }

    // 保存IP历史记录 - 只保存IP部分,不包含端口
    QString ip = addr.split(":").first();
    if (!ip.isEmpty()) {
        saveIpHistory(ip);
    }
    
    // 保存端口历史记录
    QString port = addr.split(":").last();
    if (!port.isEmpty() && port != ip) {
        savePortHistory(port);
    }

    outLog("wireless connect...", false);
    QStringList adbArgs;
    adbArgs << "connect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_startAdbdBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    outLog("start devices adbd...", false);
    // adb tcpip 5555
    QStringList adbArgs;
    adbArgs << "tcpip";
    adbArgs << "5555";
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::outLog(const QString &log, bool newLine)
{
    // avoid sub thread update ui
    QString backLog = log;
    QTimer::singleShot(0, this, [this, backLog, newLine]() {
        ui->outEdit->append(backLog);
        if (newLine) {
            ui->outEdit->append("<br/>");
        }
    });
}

bool Dialog::filterLog(const QString &log)
{
    if (log.contains("app_proces")) {
        return true;
    }
    if (log.contains("Unable to set geometry")) {
        return true;
    }
    return false;
}

bool Dialog::checkAdbRun()
{
    if (m_adb.isRuning()) {
        outLog("wait for the end of the current command to run");
    }
    return m_adb.isRuning();
}

void Dialog::on_getIPBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }

    outLog("get ip...", false);
    // adb -s P7C0218510000537 shell ifconfig wlan0
    // or
    // adb -s P7C0218510000537 shell ip -f inet addr show wlan0
    QStringList adbArgs;
#if 0
    adbArgs << "shell";
    adbArgs << "ip";
    adbArgs << "-f";
    adbArgs << "inet";
    adbArgs << "addr";
    adbArgs << "show";
    adbArgs << "wlan0";
#else
    adbArgs << "shell";
    adbArgs << "ifconfig";
    adbArgs << "wlan0";
#endif
    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::getIPbyIp()
{
    if (checkAdbRun()) {
        return;
    }

    QStringList adbArgs;
    adbArgs << "shell";
    adbArgs << "ip -o a";

    m_adb.execute(ui->serialBox->currentText().trimmed(), adbArgs);
}

void Dialog::onDeviceConnected(bool success, const QString &serial, const QString &deviceName, const QSize &size, int initialOrientation)
{
    Q_UNUSED(deviceName);
    if (!success) {
        return;
    }
    auto videoForm = new VideoForm(ui->framelessCheck->isChecked(), Config::getInstance().getSkin(), ui->showToolbar->isChecked());
    videoForm->setSerial(serial);
    videoForm->setInitialOrientationHint(initialOrientation);
    videoForm->setLocalTextInputConfig(ui->localTextInputCheck->isChecked(), ui->localTextInputShortcutEdit->keySequence());
    videoForm->setKeymapEditorShortcut(currentKeymapEditorShortcut());
    connect(videoForm, &VideoForm::restartServiceRequested, this, &Dialog::onRestartDeviceRequested);
    connect(videoForm, &QObject::destroyed, this, [this, serial]() {
        m_videoForms.remove(serial);
    });
    m_videoForms.insert(serial, videoForm);
    updateVideoFormScriptBinding(serial, getGameScriptPath(ui->gameBox->currentText()), ui->gameBox->currentText(), getGameScript(ui->gameBox->currentText()));

    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        videoForm->deleteLater();
        return;
    }

    connect(device.data(), &qsc::IDevice::recordingError, this,
            [this, serial](const QString &emittedSerial, const QString &message) {
        if (emittedSerial != serial || message.trimmed().isEmpty()) {
            return;
        }
        outLog(QString("recording error (%1): %2").arg(serial, message));
    });
    connect(device.data(), &qsc::IDevice::recordingStateChanged, this,
            [this, serial](const QString &emittedSerial, bool active, const QString &filePath) {
        if (emittedSerial != serial) {
            return;
        }
        if (active) {
            outLog(QString("recording started (%1): %2").arg(serial, filePath));
        } else {
            outLog(QString("recording stopped (%1)").arg(serial));
        }
    });

    device->setUserData(static_cast<void*>(videoForm));
    device->registerDeviceObserver(videoForm);


    videoForm->showFPS(ui->fpsCheck->isChecked());

    if (ui->alwaysTopCheck->isChecked()) {
        videoForm->staysOnTop();
    }

#ifndef Q_OS_WIN32
    // must be show before updateShowSize
    videoForm->show();
#endif
    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) {
        name = Config::getInstance().getTitle();
    }
    videoForm->setWindowTitle(name + "-" + serial);
    videoForm->updateShowSize(size);

    bool deviceVer = size.height() > size.width();
    QRect rc = Config::getInstance().getRect(serial);
    bool rcVer = rc.height() > rc.width();
    // same width/height rate
    if (rc.isValid() && (deviceVer == rcVer)) {
        // mark: resize is for fix setGeometry magneticwidget bug
        videoForm->resize(rc.size());
        videoForm->setGeometry(rc);
    }

#ifdef Q_OS_WIN32
    // windows是show太早可以看到resize的过程
    QTimer::singleShot(200, videoForm, [videoForm](){videoForm->show();});
#endif

    GroupController::instance().addDevice(serial);
}

void Dialog::onRestartDeviceRequested(const QString &serial)
{
    const QString trimmedSerial = serial.trimmed();
    if (trimmedSerial.isEmpty()) {
        outLog("restart server failed: serial is empty");
        return;
    }

    updateBootConfig(false);
    outLog(QString("restart server: %1").arg(trimmedSerial), false);

    if (!qsc::IDeviceManage::getInstance().disconnectDevice(trimmedSerial)) {
        outLog(QString("restart server failed: device not found (%1)").arg(trimmedSerial));
        return;
    }

    QTimer::singleShot(0, this, [this, trimmedSerial]() {
        const qsc::DeviceParams params = buildDeviceParams(trimmedSerial);
        if (params.serial.trimmed().isEmpty()) {
            outLog("restart server failed: serial is empty");
            return;
        }
        if (!qsc::IDeviceManage::getInstance().connectDevice(params)) {
            outLog(QString("restart server failed: connect device failed (%1)").arg(trimmedSerial));
        }
    });
}

void Dialog::onDeviceDisconnected(QString serial)
{
    m_videoForms.remove(serial);
    GroupController::instance().removeDevice(serial);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        return;
    }
    auto data = device->getUserData();
    if (data) {
        VideoForm* vf = static_cast<VideoForm*>(data);
        qsc::IDeviceManage::getInstance().getDevice(serial)->deRegisterDeviceObserver(vf);
        vf->close();
        vf->deleteLater();
    }
}

void Dialog::on_wirelessDisConnectBtn_clicked()
{
    if (checkAdbRun()) {
        return;
    }
    QString addr = ui->deviceIpEdt->currentText().trimmed();
    outLog("wireless disconnect...", false);
    QStringList adbArgs;
    adbArgs << "disconnect";
    adbArgs << addr;
    m_adb.execute("", adbArgs);
}

void Dialog::on_selectRecordPathBtn_clicked()
{
    QFileDialog::Options options = QFileDialog::DontResolveSymlinks | QFileDialog::ShowDirsOnly;
    QString directory = QFileDialog::getExistingDirectory(this, tr("select path"), "", options);
    ui->recordPathEdt->setText(directory);
}

void Dialog::on_openAppFolderBtn_clicked()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(appDir))) {
        outLog(QString("open app folder: %1").arg(appDir));
        return;
    }

    outLog(QString("could not open app folder: %1").arg(appDir));
    QMessageBox::warning(this, tr("open app folder"), tr("Could not open application folder:\n%1").arg(appDir));
}

void Dialog::on_recordPathEdt_textChanged(const QString &arg1)
{
    refreshControlToolTips();
    ui->notDisplayCheck->setCheckable(!arg1.trimmed().isEmpty());
}

void Dialog::on_adbCommandBtn_clicked()
{
    execAdbCmd();
}

void Dialog::on_stopAdbBtn_clicked()
{
    m_adb.kill();
}

void Dialog::on_clearOut_clicked()
{
    ui->outEdit->clear();
}

void Dialog::on_stopAllServerBtn_clicked()
{
    qsc::IDeviceManage::getInstance().disconnectAllDevice();
}

void Dialog::on_refreshGameScriptBtn_clicked()
{
    ui->gameBox->clear();
    QDir dir(getKeyMapPath());
    if (!dir.exists()) {
        outLog("keymap directory not find", true);
        return;
    }
    dir.setFilter(QDir::Files | QDir::NoSymLinks);
    QFileInfoList list = dir.entryInfoList();
    QFileInfo fileInfo;
    int size = list.size();
    for (int i = 0; i < size; ++i) {
        fileInfo = list.at(i);
        ui->gameBox->addItem(fileInfo.fileName());
    }
}

void Dialog::on_applyScriptBtn_clicked()
{
    auto curSerial = ui->serialBox->currentText().trimmed();
    auto device = qsc::IDeviceManage::getInstance().getDevice(curSerial);
    if (!device) {
        return;
    }

    const QString scriptJson = getGameScript(ui->gameBox->currentText());
    device->updateScript(scriptJson);
    updateVideoFormScriptBinding(curSerial, getGameScriptPath(ui->gameBox->currentText()), ui->gameBox->currentText(), scriptJson);
}

void Dialog::on_recordScreenCheck_clicked(bool checked)
{
    if (!checked) {
        return;
    }

    QString fileDir(ui->recordPathEdt->text().trimmed());
    if (fileDir.isEmpty()) {
        qWarning() << "please select record save path!!!";
        ui->recordScreenCheck->setChecked(false);
    }
}

void Dialog::on_localTextInputCheck_toggled(bool checked)
{
    ui->localTextInputShortcutEdit->setEnabled(checked);
    refreshControlToolTips();
    applyLocalTextInputConfigToOpenVideoForms();
}

void Dialog::on_localTextInputShortcutEdit_keySequenceChanged(const QKeySequence &keySequence)
{
    Q_UNUSED(keySequence);
    refreshControlToolTips();
    applyLocalTextInputConfigToOpenVideoForms();
}

void Dialog::on_keymapEditorShortcutEdit_keySequenceChanged(const QKeySequence &keySequence)
{
    Q_UNUSED(keySequence);
    if (m_keymapEditorShortcutEdit) {
        const QKeySequence normalized = normalizeSingleShortcut(m_keymapEditorShortcutEdit->keySequence());
        if (normalized != m_keymapEditorShortcutEdit->keySequence()) {
            const QSignalBlocker blocker(m_keymapEditorShortcutEdit);
            m_keymapEditorShortcutEdit->setKeySequence(normalized);
        }
    }
    refreshControlToolTips();
    applyKeymapEditorShortcutToOpenVideoForms();
}

void Dialog::on_usbConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);
    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_startServerBtn_clicked();
}

int Dialog::findDeviceFromeSerialBox(bool wifi)
{
    QString regStr = "\\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\:([0-9]|[1-9]\\d|[1-9]\\d{2}|[1-9]\\d{3}|[1-5]\\d{4}|6[0-4]\\d{3}|65[0-4]\\d{2}|655[0-2]\\d|6553[0-5])\\b";
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QRegExp regIP(regStr);
#else
    QRegularExpression regIP(regStr);
#endif
    for (int i = 0; i < ui->serialBox->count(); ++i) {
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        bool isWifi = regIP.exactMatch(ui->serialBox->itemText(i));
#else
        bool isWifi = regIP.match(ui->serialBox->itemText(i)).hasMatch();
#endif
        bool found = wifi ? isWifi : !isWifi;
        if (found) {
            return i;
        }
    }

    return -1;
}

void Dialog::on_wifiConnectBtn_clicked()
{
    on_stopAllServerBtn_clicked();
    delayMs(200);

    on_updateDevice_clicked();
    delayMs(200);

    int firstUsbDevice = findDeviceFromeSerialBox(false);
    if (-1 == firstUsbDevice) {
        qWarning() << "No use device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstUsbDevice);

    on_getIPBtn_clicked();
    delayMs(200);

    on_startAdbdBtn_clicked();
    delayMs(1000);

    on_wirelessConnectBtn_clicked();
    delayMs(2000);

    on_updateDevice_clicked();
    delayMs(200);

    int firstWifiDevice = findDeviceFromeSerialBox(true);
    if (-1 == firstWifiDevice) {
        qWarning() << "No wifi device is found!";
        return;
    }
    ui->serialBox->setCurrentIndex(firstWifiDevice);

    on_startServerBtn_clicked();
}

void Dialog::on_connectedPhoneList_itemDoubleClicked(QListWidgetItem *item)
{
    Q_UNUSED(item);
    ui->serialBox->setCurrentIndex(ui->connectedPhoneList->currentRow());
    on_startServerBtn_clicked();
}

void Dialog::on_updateNameBtn_clicked()
{
    if (ui->serialBox->count() != 0) {
        if (ui->userNameEdt->text().isEmpty()) {
            Config::getInstance().setNickName(ui->serialBox->currentText(), "Phone");
        } else {
            Config::getInstance().setNickName(ui->serialBox->currentText(), ui->userNameEdt->text());
        }

        handleSelectedSerialChanged(currentSelectedSerial());

        on_updateDevice_clicked();

        qDebug() << "Update OK!";
    } else {
        qWarning() << "No device is connected!";
    }
}

void Dialog::on_useSingleModeCheck_clicked()
{
    if (ui->useSingleModeCheck->isChecked()) {
        ui->rightWidget->hide();
    } else {
        ui->rightWidget->show();
    }

    adjustSize();
}

void Dialog::handleSelectedSerialChanged(const QString &serial)
{
    ui->userNameEdt->setText(Config::getInstance().getNickName(serial));
    updateSelectedDeviceConfigUi(serial);
}

quint32 Dialog::getBitRate()
{
    return ui->bitRateEdit->text().trimmed().toUInt() *
            (ui->bitRateBox->currentText() == QString("Mbps") ? 1000000 : 1000);
}

qsc::DeviceParams Dialog::buildDeviceParams(const QString &serial)
{
    // this is ok that "original" toUshort is 0
    quint16 videoSize = ui->maxSizeBox->currentText().trimmed().toUShort();
    const QString trimmedSerial = serial.trimmed();
    if (!trimmedSerial.isEmpty()) {
        Config::getInstance().ensureDeviceMouseConfigInitialized(trimmedSerial);
    }
    qsc::DeviceParams params;
    params.serial = trimmedSerial;
    params.maxSize = videoSize;
    params.bitRate = getBitRate();
    params.maxFps = static_cast<quint32>(qBound(0, ui->maxFpsSpin->value(), 240));
    params.closeScreen = ui->closeScreenCheck->isChecked();
    params.useReverse = ui->useReverseCheck->isChecked();
    params.display = !ui->notDisplayCheck->isChecked();
    params.renderExpiredFrames = Config::getInstance().getRenderExpiredFrames();
    if (ui->lockOrientationBox->currentIndex() > 0) {
        params.captureOrientationLock = 1;
        params.captureOrientation = (ui->lockOrientationBox->currentIndex() - 1) * 90;
    }
    params.stayAwake = ui->stayAwakeCheck->isChecked();
    params.recordFile = ui->recordScreenCheck->isChecked();
    params.recordPath = ui->recordPathEdt->text().trimmed();
    params.recordFileFormat = ui->formatBox->currentText().trimmed();
    params.serverLocalPath = getServerPath();
    params.serverRemotePath = Config::getInstance().getServerPath();
    params.pushFilePath = Config::getInstance().getPushFilePath();
    params.gameScript = getGameScript(ui->gameBox->currentText());
    params.logLevel = Config::getInstance().getLogLevel();
    params.codecOptions = Config::getInstance().getCodecOptions();
    params.codecName = Config::getInstance().getCodecName();
    params.scid = QRandomGenerator::global()->bounded(1, 10000) & 0x7FFFFFFF;
    return params;
}

void Dialog::applyLocalTextInputConfigToOpenVideoForms()
{
    const bool enabled = ui->localTextInputCheck->isChecked();
    const QKeySequence shortcut = ui->localTextInputShortcutEdit->keySequence();
    for (auto it = m_videoForms.begin(); it != m_videoForms.end();) {
        if (it.value().isNull()) {
            it = m_videoForms.erase(it);
            continue;
        }
        it.value()->setLocalTextInputConfig(enabled, shortcut);
        ++it;
    }
}

void Dialog::applyKeymapEditorShortcutToOpenVideoForms()
{
    const QKeySequence shortcut = currentKeymapEditorShortcut();
    for (auto it = m_videoForms.begin(); it != m_videoForms.end();) {
        if (it.value().isNull()) {
            it = m_videoForms.erase(it);
            continue;
        }
        it.value()->setKeymapEditorShortcut(shortcut);
        ++it;
    }
}

void Dialog::updateVideoFormScriptBinding(const QString &serial, const QString &scriptFilePath, const QString &scriptDisplayName, const QString &scriptJson)
{
    auto it = m_videoForms.find(serial);
    if (it == m_videoForms.end() || it.value().isNull()) {
        return;
    }
    it.value()->setScriptBinding(scriptFilePath, scriptDisplayName, scriptJson);
}

const QString &Dialog::getServerPath()
{
    static QString serverPath;
    if (serverPath.isEmpty()) {
        serverPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_SERVER_PATH"));
        QFileInfo fileInfo(serverPath);
        if (serverPath.isEmpty() || !fileInfo.isFile()) {
            serverPath = QCoreApplication::applicationDirPath() + "/scrcpy-server";
        }
    }
    return serverPath;
}

void Dialog::on_startAudioBtn_clicked()
{
    if (ui->serialBox->count() == 0) {
        qWarning() << "No device is connected!";
        return;
    }

    m_audioOutput.start(ui->serialBox->currentText(), 28200);
}

void Dialog::on_stopAudioBtn_clicked()
{
    m_audioOutput.stop();
}

void Dialog::on_installSndcpyBtn_clicked()
{
    if (ui->serialBox->count() == 0) {
        qWarning() << "No device is connected!";
        return;
    }
    m_audioOutput.installonly(ui->serialBox->currentText(), 28200);
}

void Dialog::on_autoUpdatecheckBox_toggled(bool checked)
{
    Q_UNUSED(checked);
    applyAutoUpdateTimerState();
    refreshControlToolTips();
}

void Dialog::on_autoUpdateIntervalSpin_valueChanged(int value)
{
    Q_UNUSED(value);
    applyAutoUpdateTimerState();
    refreshControlToolTips();
}

void Dialog::loadIpHistory()
{
    QStringList ipList = Config::getInstance().getIpHistory();
    ui->deviceIpEdt->clear();
    ui->deviceIpEdt->addItems(ipList);
    ui->deviceIpEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->deviceIpEdt->lineEdit()) {
        ui->deviceIpEdt->lineEdit()->setMaxLength(128);
        ui->deviceIpEdt->lineEdit()->setPlaceholderText("192.168.0.1");
    }
}

void Dialog::saveIpHistory(const QString &ip)
{
    if (ip.isEmpty()) {
        return;
    }
    
    Config::getInstance().saveIpHistory(ip);
    
    // 更新ComboBox
    loadIpHistory();
    ui->deviceIpEdt->setCurrentText(ip);
}

void Dialog::showIpEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->deviceIpEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearIpHistory();
        loadIpHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->deviceIpEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

void Dialog::loadPortHistory()
{
    QStringList portList = Config::getInstance().getPortHistory();
    ui->devicePortEdt->clear();
    ui->devicePortEdt->addItems(portList);
    ui->devicePortEdt->setContentsMargins(0, 0, 0, 0);

    if (ui->devicePortEdt->lineEdit()) {
        ui->devicePortEdt->lineEdit()->setMaxLength(6);
        ui->devicePortEdt->lineEdit()->setPlaceholderText("5555");
    }
}

void Dialog::savePortHistory(const QString &port)
{
    if (port.isEmpty()) {
        return;
    }
    
    Config::getInstance().savePortHistory(port);
    
    // 更新ComboBox
    loadPortHistory();
    ui->devicePortEdt->setCurrentText(port);
}

void Dialog::showPortEditMenu(const QPoint &pos)
{
    QMenu *menu = ui->devicePortEdt->lineEdit()->createStandardContextMenu();
    menu->addSeparator();
    
    QAction *clearHistoryAction = new QAction(tr("Clear History"), menu);
    connect(clearHistoryAction, &QAction::triggered, this, [this]() {
        Config::getInstance().clearPortHistory();
        loadPortHistory();
    });
    
    menu->addAction(clearHistoryAction);
    menu->exec(ui->devicePortEdt->lineEdit()->mapToGlobal(pos));
    delete menu;
}

void Dialog::restartApplication()
{
    const QString program = QCoreApplication::applicationFilePath();
    const QStringList args = QCoreApplication::arguments().mid(1);
    const QString workdir = QCoreApplication::applicationDirPath();

    if (QProcess::startDetached(program, args, workdir)) {
        outLog(QString("restart application: %1").arg(program));
        quitApplicationDirectly();
        return;
    }

    const QString error = QString("restart application failed: %1").arg(program);
    outLog(error);
    QMessageBox::warning(this, tr("restart"), error);
}

void Dialog::quitApplicationDirectly()
{
    if (m_hideIcon) {
        m_hideIcon->hide();
    }
    qApp->quit();
}
