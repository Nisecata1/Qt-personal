// #include <QDesktopWidget>
#include <QCursor>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QHostAddress>
#include <QLineEdit>
#include <QSettings>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QWindow>
#include <QtWidgets/QHBoxLayout>
#include <cmath>
#include <cstring>
#include <functional>

#if defined(Q_OS_WIN32)
#include <Windows.h>
#include "../util/winutils.h"
#endif

#include "config.h"
#include "iconhelper.h"
#include "keymapeditor/keymapeditordocument.h"
#include "keymapeditor/keymapeditoroverlay.h"
#include "keymapeditor/keymapeditorpanel.h"
#include "qyuvopenglwidget.h"
#include "thememanager.h"
#include "toolform.h"
#include "mousetap/mousetap.h"
#include "ui_videoform.h"
#include "videoform.h"

namespace {
constexpr qreal kRawSyntheticGlobalSentinel = -1000000.0;
constexpr int kRawInputSendHzMin = 60;
constexpr int kRawInputSendHzMax = 1000;
constexpr double kRawInputScaleMin = 0.1;
constexpr double kRawInputScaleMax = 50.0;
constexpr int kOrientationPollIntervalMs = 2000;
constexpr int kOrientationProbeStepTimeoutMs = 450;
constexpr int kOrientationProbeBudgetTimeoutMs = 1500;
constexpr int kRelativeLookConfigDebounceMs = 120;
constexpr quint32 kAiDeltaMagic = 0x31444941U; // "AID1" little-endian
constexpr quint16 kAiDeltaVersion = 1;
constexpr quint16 kAiUdpPort = 12345;

class LocalTextInputOverlay final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;

    std::function<void()> onEscapePressed;
    std::function<void()> onPassiveFocusOut;

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event && event->key() == Qt::Key_Escape) {
            if (onEscapePressed) {
                onEscapePressed();
            }
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        if (onPassiveFocusOut) {
            onPassiveFocusOut();
        }
    }
};

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

QRect buildCenteredCropRect(const QSize &canvasSize, int cropSize)
{
    if (!canvasSize.isValid()) {
        return QRect();
    }

    if (cropSize <= 0) {
        return QRect(QPoint(0, 0), canvasSize);
    }

    int cropW = qMin(cropSize, canvasSize.width());
    int cropH = qMin(cropSize, canvasSize.height());
    cropW &= ~1;
    cropH &= ~1;
    if (cropW < 2 || cropH < 2) {
        return QRect(QPoint(0, 0), canvasSize);
    }

    const int maxX = qMax(0, canvasSize.width() - cropW);
    const int maxY = qMax(0, canvasSize.height() - cropH);

    int x = (canvasSize.width() - cropW) / 2;
    int y = (canvasSize.height() - cropH) / 2;
    x = qBound(0, x & ~1, maxX);
    y = qBound(0, y & ~1, maxY);

    return QRect(x, y, cropW, cropH);
}

bool isLandscapeSize(const QSize &size)
{
    return size.isValid() && !size.isEmpty() && size.width() > size.height();
}

bool normalizeRotationValue(const QString &captured, int &orientationOut)
{
    bool ok = false;
    const int value = captured.toInt(&ok);
    if (!ok) {
        return false;
    }

    if (value >= 0 && value <= 3) {
        orientationOut = value;
        return true;
    }

    if ((value % 90) == 0 && value >= 0 && value <= 270) {
        orientationOut = value / 90;
        return true;
    }

    return false;
}

bool captureRegexOrientation(const QString &text, const QRegularExpression &re, int &orientationOut, bool preferLast)
{
    QRegularExpressionMatchIterator it = re.globalMatch(text);
    bool matched = false;
    int lastOrientation = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        int candidate = 0;
        if (!normalizeRotationValue(match.captured(1), candidate)) {
            continue;
        }
        if (!preferLast) {
            orientationOut = candidate;
            return true;
        }
        lastOrientation = candidate;
        matched = true;
    }

    if (matched) {
        orientationOut = lastOrientation;
    }
    return matched;
}

bool parseWindowDisplaysOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression currentRotationRe(
        R"(mCurrentRotation\s*=\s*ROTATION_([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, currentRotationRe, orientationOut, true)) {
        return true;
    }

    static const QRegularExpression displayFramesRe(
        R"(DisplayFrames[^\n]*\br\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, displayFramesRe, orientationOut, true)) {
        return true;
    }

    static const QRegularExpression rotationRe(
        R"(\bmRotation\s*=\s*(?:ROTATION_)?([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    return captureRegexOrientation(text, rotationRe, orientationOut, true);
}

bool parseDisplayOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression currentOrientationRe(
        R"(mCurrentOrientation\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, currentOrientationRe, orientationOut, false)) {
        return true;
    }

    static const QRegularExpression displayInfoRotationRe(
        R"(DisplayDeviceInfo\{".*?",.*?\brotation\s+([0-9]{1,3}),\s+type\s+INTERNAL)",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    return captureRegexOrientation(text, displayInfoRotationRe, orientationOut, false);
}

bool parseInputOrientation(const QString &text, int &orientationOut)
{
    static const QRegularExpression viewportOrientationRe(
        R"(Viewport\s+INTERNAL:[^\n]*\borientation\s*=\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    if (captureRegexOrientation(text, viewportOrientationRe, orientationOut, false)) {
        return true;
    }

    static const QRegularExpression surfaceOrientationRe(
        R"(SurfaceOrientation\s*:\s*([0-9]{1,3}))",
        QRegularExpression::CaseInsensitiveOption);
    return captureRegexOrientation(text, surfaceOrientationRe, orientationOut, true);
}

QRect keymapEditorAvailableGeometry(const QWidget *anchor)
{
    QScreen *screen = nullptr;
    if (anchor) {
        screen = anchor->screen();
        if (!screen) {
            screen = QGuiApplication::screenAt(anchor->frameGeometry().center());
        }
    }
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
}

bool keymapEditorGeometryIntersectsVisibleScreen(const QRect &rect)
{
    if (!rect.isValid() || rect.isEmpty()) {
        return false;
    }

    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *screen : screens) {
        if (screen && screen->availableGeometry().intersects(rect)) {
            return true;
        }
    }
    return false;
}
} // namespace

#pragma pack(push, 1)
struct AiDeltaPacketV1 {
    quint32 magic;
    quint16 version;
    quint16 flags;
    quint32 frameId;
    float aiDx;
    float aiDy;
};
#pragma pack(pop)
static_assert(sizeof(AiDeltaPacketV1) == 20, "AiDeltaPacketV1 size must be 20 bytes");

VideoForm::VideoForm(bool framelessWindow, bool skin, bool showToolbar, QWidget *parent) : QWidget(parent), ui(new Ui::videoForm), m_skin(skin)
{
    ui->setupUi(this);
    initUI();
    installShortcut();
    updateShowSize(size());
    bool vertical = size().height() > size().width();
    this->show_toolbar = showToolbar;
    if (m_skin) {
        updateStyleSheet(vertical);
    }
    if (framelessWindow) {
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
    }
    connect(&ThemeManager::getInstance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });
    applyTheme();
}

VideoForm::~VideoForm()
{
    shutdownKeymapEditor(false);
    stopOrientationPolling();
    releaseGrabbedCursorState();
    delete ui;
}

void VideoForm::initUI()
{
    if (m_skin) {
        QPixmap phone;
        if (phone.load(":/res/phone.png")) {
            m_widthHeightRatio = 1.0f * phone.width() / phone.height();
        }

#ifndef Q_OS_OSX
        // mac濠电姷鏁搁崑鐐哄垂閸洖绠伴柟闂寸贰閺佸嫰鏌涘☉姗嗗殶鐎规挷绶氶弻鐔煎箲閹伴潧娈紓浣哄Т閸熷潡鈥︾捄銊﹀磯濞撴凹鍨伴崜鎶芥⒑閹肩偛濡块柛妯犲棛浜遍梻浣虹帛椤ㄥ懘鎮￠崼鏇炵闁挎棁妫勬禍顖氼渻閵堝棙顥嗛柨鐔村劜缁傚秷銇愰幒鎾跺幈濠德板€曢崯顐ｇ閿曞倹鐓欐い鏍ㄧ〒瀹曠owfullscreen
        // 闂傚倸鍊风粈渚€骞夐敓鐘偓锕傚炊閳轰礁鐏婂銈嗙墬缁秹寮冲鍫熺厓鐟滄粓宕滈悢鐓庤摕闁炽儲鍓氶崥瀣煕濞戝崬鏋涢柡瀣Т椤啴濡惰箛鏇烆嚤缂備緡鍠楅悷銉╊敋?
        setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        // 闂傚倸鍊风粈渚€骞栭銈囩煋闁绘垶鏋荤紞鏍ь熆鐠虹尨鍔熼柡鍡愬€曢妴鎺戭潩閿濆懍澹曢柣搴ゎ潐濞叉牠鎮ラ悡搴ｆ殾婵犲﹤妫Σ璇差渻閵堝繒鍒版い顓犲厴瀵鏁嶉崟銊ヤ壕闁挎繂绨肩花缁樸亜韫囷絽寮柡灞界Х椤т線鏌涢幘瀵糕姇闁逛究鍔庨埀顒勬涧閹诧繝锝為弴銏＄厵闁诡垎鍛殯婵炲瓨绮岀紞濠囧蓟閺囷紕鐤€閻庯綆浜炴导鍕倵鐟欏嫭绀€闁绘牕銈稿?
        setAttribute(Qt::WA_TranslucentBackground);
#endif
    }

    m_videoWidget = new QYUVOpenGLWidget();
    m_videoWidget->hide();
    m_videoWidget->setFocusPolicy(Qt::StrongFocus);
    ui->keepRatioWidget->setWidget(m_videoWidget);
    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
    loadVideoEnabledConfig();

    m_noVideoLabel = new QLabel(ui->keepRatioWidget);
    m_noVideoLabel->setAlignment(Qt::AlignCenter);
    m_noVideoLabel->setText(tr("Pure Control Mode (Video Disabled)\nControl channel is active."));
    m_noVideoLabel->setStyleSheet(ThemeManager::getInstance().noVideoOverlayStyleSheet());
    m_noVideoLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_noVideoLabel->setFocusPolicy(Qt::NoFocus);
    m_noVideoLabel->hide();

    auto localTextInput = new LocalTextInputOverlay(ui->keepRatioWidget);
    localTextInput->setPlaceholderText(tr("Type here and press Enter"));
    localTextInput->setClearButtonEnabled(true);
    localTextInput->setFocusPolicy(Qt::StrongFocus);
    localTextInput->setAttribute(Qt::WA_InputMethodEnabled, true);
    localTextInput->setStyleSheet(ThemeManager::getInstance().localTextInputOverlayStyleSheet());
    localTextInput->hide();
    localTextInput->onEscapePressed = [this]() {
        hideLocalTextInputOverlay(true);
    };
    localTextInput->onPassiveFocusOut = [this]() {
        hideLocalTextInputOverlay(false, false);
    };
    connect(localTextInput, &QLineEdit::returnPressed, this, [this]() {
        submitLocalTextInputOverlay();
    });
    m_localTextInput = localTextInput;

    m_fpsLabel = new QLabel(m_videoWidget);
    QFont ft;
    ft.setPointSize(15);
    ft.setWeight(QFont::Light);
    ft.setBold(true);
    m_fpsLabel->setFont(ft);
    m_fpsLabel->move(5, 15);
    m_fpsLabel->setMinimumWidth(100);
    m_fpsLabel->setStyleSheet(R"(QLabel {color: #00FF00;})");

    setMouseTracking(true);
    m_videoWidget->setMouseTracking(true);
    ui->keepRatioWidget->setMouseTracking(true);

    m_rawInputSendTimer = new QTimer(this);
    connect(m_rawInputSendTimer, &QTimer::timeout, this, [this]() {
        dispatchRawInputMouseMove(false);
    });

    initAiUdpReceiver();
    initRelativeLookConfigWatcher();
    initOrientationPoller();

    if (!m_videoEnabled) {
        m_videoWidget->show();
    }
    startOrientationPollingIfNeeded();
    updateNoVideoOverlay();
}

void VideoForm::applyTheme()
{
    ThemeManager &themeManager = ThemeManager::getInstance();
    const bool darkTheme = themeManager.isDarkTheme();
    if (m_noVideoLabel) {
        m_noVideoLabel->setStyleSheet(themeManager.noVideoOverlayStyleSheet());
    }

    if (m_localTextInput) {
        m_localTextInput->setStyleSheet(themeManager.localTextInputOverlayStyleSheet());
    }

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)winId(), darkTheme);
#endif
}

QRect VideoForm::getGrabCursorRect()
{
    QRect rc;
#if defined(Q_OS_WIN32)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget->pos()), m_videoWidget->size());
    // high dpi support
    rc.setTopLeft(rc.topLeft() * m_videoWidget->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_OSX)
    rc = m_videoWidget->geometry();
    rc.setTopLeft(ui->keepRatioWidget->mapToGlobal(rc.topLeft()));
    rc.setBottomRight(ui->keepRatioWidget->mapToGlobal(rc.bottomRight()));

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#elif defined(Q_OS_LINUX)
    rc = QRect(ui->keepRatioWidget->mapToGlobal(m_videoWidget->pos()), m_videoWidget->size());
    // high dpi support -- taken from the WIN32 section and untested
    rc.setTopLeft(rc.topLeft() * m_videoWidget->devicePixelRatioF());
    rc.setBottomRight(rc.bottomRight() * m_videoWidget->devicePixelRatioF());

    rc.setX(rc.x() + 10);
    rc.setY(rc.y() + 10);
    rc.setWidth(rc.width() - 20);
    rc.setHeight(rc.height() - 20);
#endif
    return rc;
}

const QSize &VideoForm::frameSize()
{
    return m_frameSize;
}

void VideoForm::resizeSquare()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    resize(screenRect.height(), screenRect.height());
}

void VideoForm::removeBlackRect()
{
    resize(ui->keepRatioWidget->goodSize());
}

void VideoForm::showFPS(bool show)
{
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setVisible(show);
}

void VideoForm::updateRender(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)
{
    m_streamFrameSize = QSize(width, height);
    if (!m_frameSize.isValid()) {
        updateShowSize(m_streamFrameSize);
    } else if (m_videoCenterCropSize <= 0 && m_lockDirectionIndex <= 0
               && m_streamFrameSize.isValid() && !m_streamFrameSize.isEmpty()) {
        const bool streamLandscape = isLandscapeSize(m_streamFrameSize);
        const bool canvasLandscape = isLandscapeSize(m_frameSize);
        if (streamLandscape != canvasLandscape) {
            const QSize oldCanvas = m_frameSize;
            updateShowSize(m_streamFrameSize);
            qInfo() << "Stream orientation updated canvas:"
                    << "oldCanvas=" << oldCanvas
                    << "streamFrame=" << m_streamFrameSize
                    << "newCanvas=" << m_frameSize
                    << "cropSize=" << m_videoCenterCropSize;
            resetOrientationProbeState();
        }
    }

    if (m_videoWidget->isHidden()) {
        m_pendingVideoWidgetReveal = true;
        if (isVisible()) {
            if (m_loadingWidget) {
                m_loadingWidget->close();
            }
            ui->keepRatioWidget->relayoutNow();
            m_videoWidget->show();
            applyVideoCanvasLayout();
        }
    }

    m_videoWidget->setStreamFrameSize(m_streamFrameSize);
    applyVideoCanvasLayout();
    positionKeymapEditorUi();

    if (!m_videoSessionFirstFrameLogged && m_streamFrameSize.isValid() && isVisible()
        && m_videoWidget && m_videoWidget->size().isValid()
        && m_videoWidget->framebufferPixelSize().isValid()) {
        const QSize widgetLogicalSize = m_videoWidget ? m_videoWidget->size() : QSize();
        const QSize framebufferPixelSize = m_videoWidget ? m_videoWidget->framebufferPixelSize() : QSize();
        const qreal devicePixelRatio = m_videoWidget ? m_videoWidget->devicePixelRatioF() : 1.0;
        qInfo() << "Video session first frame:"
                << "widgetLogical=" << widgetLogicalSize
                << "framebufferPixel=" << framebufferPixelSize
                << "devicePixelRatio=" << devicePixelRatio
                << "sessionCanvas=" << m_frameSize.width() << "x" << m_frameSize.height()
                << "streamFrame=" << m_streamFrameSize.width() << "x" << m_streamFrameSize.height()
                << "cropSize=" << m_videoCenterCropSize
                << "controlMapToScreen=" << m_controlMapToScreen
                << "contentRect=" << m_contentRect;
        m_videoSessionFirstFrameLogged = true;
    }

    m_videoWidget->updateTextures(dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
    updateNoVideoOverlay();
}

void VideoForm::applyVideoCanvasLayout()
{
    if (!m_videoWidget) {
        return;
    }

    QSize canvasSize = m_frameSize;
    if (!canvasSize.isValid()) {
        canvasSize = m_streamFrameSize;
    }

    if (!canvasSize.isValid()) {
        return;
    }

    m_contentRect = buildCenteredCropRect(canvasSize, m_controlMapToScreen ? m_videoCenterCropSize : 0);
    if (!m_contentRect.isValid() || m_contentRect.isEmpty()) {
        m_contentRect = QRect(QPoint(0, 0), canvasSize);
    }

    m_videoWidget->setCanvasSize(canvasSize);
    m_videoWidget->setContentRect(m_contentRect);
    positionLocalTextInput();
    positionKeymapEditorUi();
}
void VideoForm::setSerial(const QString &serial)
{
    m_serial = serial;
    if (m_toolForm) {
        m_toolForm->setSerial(serial);
    }
    bindDeviceRecordingState();
    resetOrientationProbeState();
    m_pendingInitialOrientation = -1;
    reloadViewControlSeparationConfig();
    startOrientationPollingIfNeeded();
    updateNoVideoOverlay();
}

void VideoForm::setInitialOrientationHint(int orientation)
{
    m_pendingInitialOrientation = orientation;
}

void VideoForm::setLocalTextInputConfig(bool enabled, const QKeySequence &shortcut)
{
    m_localTextInputEnabled = enabled;
    m_localTextInputKeySequence = shortcut;

    if (m_localTextInputShortcut) {
        delete m_localTextInputShortcut.data();
        m_localTextInputShortcut = nullptr;
    }

    if (!m_localTextInputEnabled || m_localTextInputKeySequence.isEmpty()) {
        hideLocalTextInputOverlay(false);
        return;
    }

    auto shortcutObj = new QShortcut(m_localTextInputKeySequence, this);
    shortcutObj->setAutoRepeat(false);
    connect(shortcutObj, &QShortcut::activated, this, [this]() {
        showLocalTextInputOverlay();
    });
    m_localTextInputShortcut = shortcutObj;
    updateKeymapEditorShortcutStates();
}

void VideoForm::setKeymapEditorShortcut(const QKeySequence &shortcut)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_keymapEditorKeySequence = (shortcut.isEmpty() || shortcut[0] == QKeyCombination())
        ? QKeySequence()
        : QKeySequence(shortcut[0]);
#else
    m_keymapEditorKeySequence = (shortcut.isEmpty() || shortcut[0] == 0)
        ? QKeySequence()
        : QKeySequence(shortcut[0]);
#endif

    if (m_toggleKeymapEditorShortcut) {
        delete m_toggleKeymapEditorShortcut.data();
        m_toggleKeymapEditorShortcut = nullptr;
    }

    if (!m_keymapEditorKeySequence.isEmpty()) {
        auto *shortcutObj = new QShortcut(m_keymapEditorKeySequence, this);
        shortcutObj->setAutoRepeat(false);
        connect(shortcutObj, &QShortcut::activated, this, [this]() {
            if (isKeymapEditorActive()) {
                shutdownKeymapEditor(true);
            } else {
                enterKeymapEditor();
            }
        });
        m_toggleKeymapEditorShortcut = shortcutObj;
    }

    if (m_keymapEditorPanel) {
        m_keymapEditorPanel->setCloseShortcut(m_keymapEditorKeySequence);
    }

    updateKeymapEditorShortcutStates();
}

void VideoForm::setScriptBinding(const QString &filePath, const QString &displayName, const QString &json)
{
    m_scriptFilePath = filePath;
    m_scriptDisplayName = displayName;
    m_lastAppliedScriptJson = json;

    if (!m_keymapEditorDocument || !m_keymapEditorActive || m_keymapEditorDocument->isDirty()) {
        return;
    }

    QString errorString;
    if (!m_keymapEditorDocument->loadFromJson(m_lastAppliedScriptJson, m_scriptFilePath, m_scriptDisplayName, &errorString)) {
        qWarning() << "Failed to reload keymap editor document from updated script binding:"
                   << "serial=" << m_serial
                   << "filePath=" << m_scriptFilePath
                   << "error=" << errorString;
        return;
    }

    if (m_keymapEditorPanel) {
        m_keymapEditorPanel->setScriptDisplayName(m_scriptDisplayName);
    }
}

void VideoForm::bindDeviceRecordingState()
{
    if (m_boundDevice) {
        disconnect(m_boundDevice.data(), nullptr, this, nullptr);
    }

    m_recordingActive = false;
    m_recordingFilePath.clear();
    m_boundDevice = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_boundDevice) {
        connect(m_boundDevice.data(), &qsc::IDevice::recordingStateChanged,
                this, &VideoForm::handleRecordingStateChanged);
        connect(m_boundDevice.data(), &qsc::IDevice::recordingError,
                this, &VideoForm::handleRecordingError);
        m_recordingActive = m_boundDevice->isRecording();
    }

    refreshToolFormRecordingState();
}

void VideoForm::refreshToolFormRecordingState()
{
    if (m_toolForm) {
        m_toolForm->setRecordingState(m_recordingActive, m_recordingFilePath);
    }
}

QString VideoForm::currentRecordFormat() const
{
    const UserBootConfig config = Config::getInstance().getUserBootConfig();
    switch (config.recordFormatIndex) {
    case 1:
        return QStringLiteral("mkv");
    case 0:
    default:
        return QStringLiteral("mp4");
    }
}

void VideoForm::startRecordingFromToolForm()
{
    if (!m_boundDevice) {
        QMessageBox::warning(this, tr("录制屏幕"), tr("当前设备会话不可用，无法开始录制。"));
        return;
    }

    const UserBootConfig config = Config::getInstance().getUserBootConfig();
    const QString recordPath = config.recordPath.trimmed();
    if (recordPath.isEmpty()) {
        QMessageBox::warning(this, tr("录制屏幕"), tr("请先在主窗口选择录屏保存目录。"));
        return;
    }

    const QString format = currentRecordFormat();
    if (format.isEmpty()) {
        QMessageBox::warning(this, tr("录制屏幕"), tr("当前录制格式无效，请在主窗口重新选择录制格式。"));
        return;
    }

    QString errorString;
    if (!m_boundDevice->startRecording(recordPath, format, &errorString) && !errorString.isEmpty()) {
        QMessageBox::warning(this, tr("录制屏幕"), errorString);
    }
}

void VideoForm::stopRecordingFromToolForm()
{
    if (!m_boundDevice) {
        return;
    }

    m_boundDevice->stopRecording();
}

void VideoForm::handleRecordingStateChanged(const QString &serial, bool active, const QString &filePath)
{
    if (serial != m_serial) {
        return;
    }

    m_recordingActive = active;
    m_recordingFilePath = active ? filePath : QString();
    refreshToolFormRecordingState();
}

void VideoForm::handleRecordingError(const QString &serial, const QString &message)
{
    if (serial != m_serial || message.trimmed().isEmpty()) {
        return;
    }

    m_recordingActive = false;
    m_recordingFilePath.clear();
    refreshToolFormRecordingState();
    QMessageBox::warning(this, tr("录制屏幕"), message);
}

void VideoForm::showToolForm(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->setSerial(m_serial);
        connect(m_toolForm, &ToolForm::restartServiceRequested, this, [this]() {
            emit restartServiceRequested(m_serial);
        });
        connect(m_toolForm, &ToolForm::startRecordingRequested, this, [this]() {
            startRecordingFromToolForm();
        });
        connect(m_toolForm, &ToolForm::stopRecordingRequested, this, [this]() {
            stopRecordingFromToolForm();
        });
        refreshToolFormRecordingState();
    }
    m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    m_toolForm->setVisible(show && !isKeymapEditorActive());
}

void VideoForm::moveCenter()
{
    QRect screenRect = getScreenRect();
    if (screenRect.isEmpty()) {
        qWarning() << "getScreenRect is empty";
        return;
    }
    // 缂傚倸鍊搁崐鐑芥倿閿曞倸绀夐柡宥庡幑閳ь剙鍟村畷銊╂嚋椤戞寧鐫忔繝鐢靛仦閸ㄥ爼鎯岄灏栨煢妞ゅ繐鐗婇悡鏇熺箾閹存繂鑸瑰褎鐓￠弻?
    move(screenRect.center() - QRect(0, 0, size().width(), size().height()).center());
}

void VideoForm::installShortcut()
{
    auto registerStandardShortcut = [this](const QKeySequence &sequence, const std::function<void()> &handler, bool autoRepeat = false) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setAutoRepeat(autoRepeat);
        connect(shortcut, &QShortcut::activated, this, [handler]() {
            handler();
        });
        m_standardShortcuts.push_back(shortcut);
    };

    registerStandardShortcut(QKeySequence("Ctrl+f"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });
    registerStandardShortcut(QKeySequence("Ctrl+g"), [this]() { resizeSquare(); });
    registerStandardShortcut(QKeySequence("Ctrl+w"), [this]() { removeBlackRect(); });
    registerStandardShortcut(QKeySequence("Ctrl+h"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            device->postGoHome();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+b"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            device->postGoBack();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+s"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postAppSwitch();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+m"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            device->postGoMenu();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+up"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postVolumeUp();
        }
    }, true);
    registerStandardShortcut(QKeySequence("Ctrl+down"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postVolumeDown();
        }
    }, true);
    registerStandardShortcut(QKeySequence("Ctrl+p"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postPower();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+o"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->setDisplayPower(false);
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+n"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->expandNotificationPanel();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+Shift+n"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->collapsePanel();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+c"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postCopy();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+x"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->postCut();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+v"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->setDeviceClipboard();
        }
    });
    registerStandardShortcut(QKeySequence("Ctrl+Shift+v"), [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (device) {
            emit device->clipboardPaste();
        }
    });

    setKeymapEditorShortcut(m_keymapEditorKeySequence);
}

void VideoForm::ensureKeymapEditorUi()
{
    if (!m_keymapEditorDocument) {
        m_keymapEditorDocument = new KeymapEditorDocument(this);
    }

    if (!m_keymapEditorOverlay) {
        m_keymapEditorOverlay = new KeymapEditorOverlay(ui->keepRatioWidget);
        m_keymapEditorOverlay->hide();
        connect(m_keymapEditorOverlay, &KeymapEditorOverlay::nodeSelected, this, [this](int nodeId) {
            if (m_keymapEditorPanel) {
                m_keymapEditorPanel->setSelectedNodeId(nodeId);
            }
        });
    }

    if (!m_keymapEditorPanel) {
        m_keymapEditorPanel = new KeymapEditorPanel(this);
        m_keymapEditorPanel->hide();
        connect(m_keymapEditorPanel, &KeymapEditorPanel::nodeSelected, this, [this](int nodeId) {
            if (m_keymapEditorOverlay) {
                m_keymapEditorOverlay->setSelectedNodeId(nodeId);
            }
        });
        connect(m_keymapEditorPanel, &KeymapEditorPanel::saveRequested, this, [this]() {
            saveAndApplyKeymapEditor();
        });
        connect(m_keymapEditorPanel, &KeymapEditorPanel::discardRequested, this, [this]() {
            shutdownKeymapEditor(false);
        });
        connect(m_keymapEditorPanel, &KeymapEditorPanel::closeRequested, this, [this]() {
            shutdownKeymapEditor(true);
        });
    }

    m_keymapEditorOverlay->setDocument(m_keymapEditorDocument);
    m_keymapEditorPanel->setDocument(m_keymapEditorDocument);
    m_keymapEditorPanel->setCloseShortcut(m_keymapEditorKeySequence);
}

void VideoForm::positionKeymapEditorUi()
{
    if (!m_keymapEditorOverlay) {
        return;
    }

    QRect overlayRect = m_videoWidget ? m_videoWidget->geometry() : QRect();
    if (!overlayRect.isValid() || overlayRect.isEmpty()) {
        overlayRect = ui->keepRatioWidget->rect();
    }
    m_keymapEditorOverlay->setGeometry(overlayRect);
    m_keymapEditorOverlay->raise();
}

QRect VideoForm::defaultKeymapEditorPanelGeometry() const
{
    const int panelWidth = 340;
    const int panelMargin = 14;
    const int panelHeight = qMax(320, height() - panelMargin * 2);

    QRect desired(frameGeometry().right() + panelMargin + 1,
                  frameGeometry().top() + panelMargin,
                  panelWidth,
                  panelHeight);
    return normalizeKeymapEditorPanelGeometry(desired);
}

QRect VideoForm::normalizeKeymapEditorPanelGeometry(const QRect &requested) const
{
    QRect available = keymapEditorAvailableGeometry(this);
    if (!available.isValid() || available.isEmpty()) {
        return requested;
    }

    QRect normalized = requested;
    normalized.setWidth(qMin(normalized.width(), available.width()));
    normalized.setHeight(qMin(normalized.height(), available.height()));
    if (normalized.width() < 320) {
        normalized.setWidth(qMin(320, available.width()));
    }
    if (normalized.height() < 320) {
        normalized.setHeight(qMin(320, available.height()));
    }

    if (normalized.left() < available.left()) {
        normalized.moveLeft(available.left());
    }
    if (normalized.top() < available.top()) {
        normalized.moveTop(available.top());
    }
    if (normalized.right() > available.right()) {
        normalized.moveRight(available.right());
    }
    if (normalized.bottom() > available.bottom()) {
        normalized.moveBottom(available.bottom());
    }

    if (normalized.left() < available.left()) {
        normalized.moveLeft(available.left());
    }
    if (normalized.top() < available.top()) {
        normalized.moveTop(available.top());
    }
    return normalized;
}

void VideoForm::restoreKeymapEditorPanelGeometry()
{
    if (!m_keymapEditorPanel) {
        return;
    }

    QRect panelRect = Config::getInstance().getKeymapEditorRect(m_serial);
    if (!panelRect.isValid() || panelRect.isEmpty() || !keymapEditorGeometryIntersectsVisibleScreen(panelRect)) {
        panelRect = defaultKeymapEditorPanelGeometry();
    }
    m_keymapEditorPanel->setGeometry(normalizeKeymapEditorPanelGeometry(panelRect));
}

void VideoForm::saveKeymapEditorPanelGeometry()
{
    if (!m_keymapEditorPanel || m_serial.trimmed().isEmpty()) {
        return;
    }

    const QRect rect = m_keymapEditorPanel->geometry();
    if (!rect.isValid() || rect.isEmpty()) {
        return;
    }
    Config::getInstance().setKeymapEditorRect(m_serial, rect);
}

void VideoForm::setKeymapEditorActive(bool active)
{
    if (m_keymapEditorActive == active) {
        return;
    }

    m_keymapEditorActive = active;
    if (active) {
        hideLocalTextInputOverlay(false);
        if (m_toolForm) {
            m_toolForm->hide();
        }
        ensureKeymapEditorUi();
        positionKeymapEditorUi();
        restoreKeymapEditorPanelGeometry();
        m_keymapEditorOverlay->show();
        m_keymapEditorOverlay->raise();
        m_keymapEditorPanel->show();
        m_keymapEditorPanel->raise();
        m_keymapEditorPanel->activateWindow();
    } else {
        if (m_keymapEditorOverlay) {
            m_keymapEditorOverlay->hide();
        }
        if (m_keymapEditorPanel) {
            saveKeymapEditorPanelGeometry();
            m_keymapEditorPanel->hide();
        }
        if (!isFullScreen() && show_toolbar) {
            showToolForm(true);
        }
    }

    updateKeymapEditorShortcutStates();
}

void VideoForm::enterKeymapEditor()
{
    if (isKeymapEditorActive()) {
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    if (m_cursorGrabbed) {
        releaseGrabbedCursorState();
    }
    hideLocalTextInputOverlay(false);

    QString scriptJson = m_lastAppliedScriptJson;
    if (scriptJson.isEmpty() && !m_scriptFilePath.isEmpty()) {
        QFile file(m_scriptFilePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            scriptJson = QString::fromUtf8(file.readAll());
        }
    }

    if (m_scriptFilePath.trimmed().isEmpty() || scriptJson.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("keymap editor"), tr("No applied script is bound to this window."));
        return;
    }

    ensureKeymapEditorUi();

    QString errorString;
    if (!m_keymapEditorDocument->loadFromJson(scriptJson, m_scriptFilePath, m_scriptDisplayName, &errorString)) {
        QMessageBox::warning(this, tr("keymap editor"), tr("Could not load script:\n%1").arg(errorString));
        return;
    }

    m_keymapEditorPanel->setScriptDisplayName(m_scriptDisplayName);
    const QVector<KeymapEditorDocument::NodeInfo> nodes = m_keymapEditorDocument->nodeInfos();
    const int selectedNodeId = nodes.isEmpty() ? -1 : nodes.first().id;
    m_keymapEditorOverlay->setSelectedNodeId(selectedNodeId);
    m_keymapEditorPanel->setSelectedNodeId(selectedNodeId);
    setKeymapEditorActive(true);
}

void VideoForm::shutdownKeymapEditor(bool promptForUnsavedChanges)
{
    if (!isKeymapEditorActive()) {
        return;
    }

    if (promptForUnsavedChanges && m_keymapEditorDocument && m_keymapEditorDocument->isDirty()) {
        QWidget *boxParent = (m_keymapEditorPanel && m_keymapEditorPanel->isVisible())
            ? static_cast<QWidget *>(m_keymapEditorPanel.data())
            : static_cast<QWidget *>(this);
        QMessageBox box(boxParent);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("keymap editor"));
        box.setText(tr("Script has unsaved changes."));
        QPushButton *saveButton = box.addButton(tr("Save && Apply"), QMessageBox::AcceptRole);
        QPushButton *discardButton = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        QPushButton *cancelButton = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();

        if (box.clickedButton() == saveButton) {
            if (!saveAndApplyKeymapEditor()) {
                return;
            }
        } else if (box.clickedButton() == cancelButton || box.clickedButton() == nullptr) {
            return;
        }
    }

    setKeymapEditorActive(false);
}

bool VideoForm::saveAndApplyKeymapEditor()
{
    if (!m_keymapEditorDocument || !m_keymapEditorDocument->hasLoadedDocument()) {
        return false;
    }

    QString errorString;
    if (!m_keymapEditorDocument->save(&errorString)) {
        QMessageBox::warning(this, tr("keymap editor"), tr("Could not save script:\n%1").arg(errorString));
        return false;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        QMessageBox::warning(this, tr("keymap editor"), tr("Device is no longer available."));
        return false;
    }

    m_lastAppliedScriptJson = m_keymapEditorDocument->toJsonString();
    qInfo() << "Keymap editor saved and applied:"
            << "serial=" << m_serial
            << "scriptFilePath=" << m_scriptFilePath;
    device->updateScript(m_lastAppliedScriptJson);
    setKeymapEditorActive(false);
    return true;
}

void VideoForm::updateKeymapEditorShortcutStates()
{
    const bool editorActive = isKeymapEditorActive();
    for (int i = 0; i < m_standardShortcuts.size(); ++i) {
        if (m_standardShortcuts.at(i)) {
            m_standardShortcuts.at(i)->setEnabled(!editorActive);
        }
    }
    if (m_localTextInputShortcut) {
        m_localTextInputShortcut->setEnabled(!editorActive);
    }
    if (m_toggleKeymapEditorShortcut) {
        m_toggleKeymapEditorShortcut->setEnabled(true);
    }
}

bool VideoForm::isKeymapEditorActive() const
{
    return m_keymapEditorActive;
}

QRect VideoForm::getScreenRect()
{
    QRect screenRect;
    QScreen *screen = QGuiApplication::primaryScreen();
    QWidget *win = window();
    if (win) {
        QWindow *winHandle = win->windowHandle();
        if (winHandle) {
            screen = winHandle->screen();
        }
    }

    if (screen) {
        screenRect = screen->availableGeometry();
    }
    return screenRect;
}

void VideoForm::updateStyleSheet(bool vertical)
{
    if (vertical) {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-v.png) 150px 65px 85px 65px;
                     border-width: 150px 65px 85px 65px;
                 }
                 )");
    } else {
        setStyleSheet(R"(
                 #videoForm {
                     border-image: url(:/image/videoform/phone-h.png) 65px 85px 65px 150px;
                     border-width: 65px 85px 65px 150px;
                 }
                 )");
    }
    layout()->setContentsMargins(getMargins(vertical));
}

QMargins VideoForm::getMargins(bool vertical)
{
    QMargins margins;
    if (vertical) {
        margins = QMargins(10, 68, 12, 62);
    } else {
        margins = QMargins(68, 12, 62, 10);
    }
    return margins;
}

void VideoForm::updateShowSize(const QSize &newSize)
{
    if (m_frameSize != newSize) {
        m_frameSize = newSize;
        m_widthHeightRatio = 1.0f * newSize.width() / newSize.height();
        ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
        ui->keepRatioWidget->relayoutNow();

        bool vertical = m_widthHeightRatio < 1.0f ? true : false;
        QSize showSize = newSize;
        QRect screenRect = getScreenRect();
        if (screenRect.isEmpty()) {
            qWarning() << "getScreenRect is empty";
            return;
        }
        if (vertical) {
            showSize.setHeight(qMin(newSize.height(), screenRect.height() - 200));
            showSize.setWidth(showSize.height() * m_widthHeightRatio);
        } else {
            showSize.setWidth(qMin(newSize.width(), screenRect.width() / 2));
            showSize.setHeight(showSize.width() / m_widthHeightRatio);
        }

        if (isFullScreen() && qsc::IDeviceManage::getInstance().getDevice(m_serial)) {
            switchFullScreen();
        }

        if (isMaximized()) {
            showNormal();
        }

        if (m_skin) {
            QMargins m = getMargins(vertical);
            showSize.setWidth(showSize.width() + m.left() + m.right());
            showSize.setHeight(showSize.height() + m.top() + m.bottom());
        }

        if (showSize != size()) {
            resize(showSize);
            if (m_skin) {
                updateStyleSheet(vertical);
            }
            moveCenter();
            ui->keepRatioWidget->relayoutNow();
        }
    }

    if (m_frameSize.isValid() && !m_frameSize.isEmpty()
        && m_pendingInitialOrientation >= 0
        && m_videoCenterCropSize > 0
        && m_lockDirectionIndex <= 0) {
        m_orientationBaseValue = m_pendingInitialOrientation;
        m_orientationBaseUiSize = m_frameSize;
        m_orientationBaseReady = true;
        qInfo() << "VideoForm initial orientation hint accepted:"
                << "orientation=" << m_pendingInitialOrientation
                << "canvas=" << m_frameSize
                << "cropSize=" << m_videoCenterCropSize;
        m_pendingInitialOrientation = -1;
    }
    applyVideoCanvasLayout();
    startOrientationPollingIfNeeded();
    updateNoVideoOverlay();
}

void VideoForm::switchFullScreen()
{
    if (isFullScreen()) {
        // 婵犵數濮烽。钘壩ｉ崨鏉戝瀭闁稿繗鍋愰々鍙夌節婵犲倻澧涢柛搴㈡崌閺屾盯鍩勯崘顏佹缂備讲鍋撻柛宀€鍋為悡蹇撯攽閻愯尙浠㈤柛鏂诲€栫换娑㈡偂鎼达絿鍔┑顔硷攻濡炶棄鐣峰鍡╂Щ闂佸憡鏌ㄩ鍥焵椤掍緡鍟忛柛鐕佸亰瀹曟儼顦存い蟻鍥ㄢ拺闂傚牊渚楅悞楣冩煕鎼粹€虫毐妞ゎ厼娲ら悾婵嬪礋椤掑倸寮虫繝鐢靛仦閸ㄦ儼鎽┑鐘亾闁哄锛曡ぐ鎺撳亼闁逞屽墴瀹曟澘螖閳ь剟顢氶妷鈺佺妞ゆ劦鍋勯幃鎴︽⒑缁洖澧查柨鏇楁櫅鍗辨い鏍仦閳锋垿鏌熺粙鎸庢崳闁宠棄顦甸弻锟犲醇椤愩垹顫紓浣稿€圭敮锟犮€佸▎鎾村癄濠㈣泛鐬奸悰顕€鏌ｆ惔锛勭暛闁稿氦浜埀顒佸嚬閸犳氨鍒掔紒妯碱浄閻庯綆鍋嗛崢鐢告⒒閸屾浜鹃梺褰掑亰閸ｎ喖危椤旂⒈娓婚柕鍫濇閳锋劖鎱ㄦ繝鍌滅Ш闁靛棗鍟村畷鍫曗€栭鍌氭灈闁圭绻濇俊鍫曞窗?
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);
        }

        showNormal();
        // back to normal size.
        resize(m_normalSize);
        // fullscreen window will move (0,0). qt bug?
        move(m_fullScreenBeforePos);

#ifdef Q_OS_OSX
        //setWindowFlags(windowFlags() | Qt::FramelessWindowHint);
        //show();
#endif
        if (m_skin) {
            updateStyleSheet(m_frameSize.height() > m_frameSize.width());
        }
        showToolForm(this->show_toolbar);
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS);
#endif
    } else {
        // 婵犵數濮烽。钘壩ｉ崨鏉戝瀭闁稿繗鍋愰々鍙夌節婵犲倻澧涢柛搴㈡崌閺屾盯鍩勯崘顏佹缂備讲鍋撻柛宀€鍋為悡蹇撯攽閻愯尙浠㈤柛鏂诲€栫换娑㈡偂鎼达絿鍔┑顔硷攻濡炶棄鐣峰鍡╂Щ闂佸憡鏌ㄩ鍥焵椤掍緡鍟忛柛鐕佸亰瀹曟儼顦存い蟻鍥ㄢ拺闂傚牊渚楅悞楣冩煕鎼粹€虫毐妞ゎ厼娲ら悾婵嬪礋椤掑倸寮虫繝鐢靛仦閸ㄥ爼鏁嬪銈冨妽閻熝呮閹烘嚦鏃堝焵椤掑媻鍥箥椤斿墽鐓旈梺鍛婎殘閸嬫劙寮ㄦ禒瀣厱闁靛鍨甸幊蹇撴毄闂傚倷娴囬褔鏌婇敐澶婄劦妞ゆ帊鑳堕妴鎺楁煟椤撶喓鎳囬柡宀嬬到閳规垿骞囬浣轰邯闁?
        if (m_widthHeightRatio > 1.0f) {
            ui->keepRatioWidget->setWidthHeightRatio(-1.0f);
        }

        // record current size before fullscreen, it will be used to rollback size after exit fullscreen.
        m_normalSize = size();

        m_fullScreenBeforePos = pos();
        // 闂傚倷绀侀幖顐λ囬锕€鐤炬繝濠傜墛閸嬶紕鎲搁弮鍫熸櫜闁绘劕鎼粻鎶芥煙閹呬邯闁哄鐗犻弻锝嗘償閵忊懇濮囬柦鍐哺閵囧嫯鐔侀柛鎰⒔閸炵敻鎮峰鍐鐎规洘鍨甸埥澶娾枎閹邦剙浼庨梻浣虹帛閸旀洟骞栭锕€绀冮柍褜鍓熷娲箹閻愭彃濮堕梺鍛婃惈缁犳挸顕ｉ幎绛嬫晬闁绘劕顕崢鎼佹倵楠炲灝鍔氶柟铏姉缁粯瀵肩€涙鍘遍梺缁樻⒐瑜板啯绂嶆ィ鍐┾拻濞达絼璀﹂悞鍓х磼鐠囪尙澧︾€规洘绻傞悾婵嬪礋椤掆偓閸撶敻姊虹化鏇炲⒉缂佸甯″畷鏇烆吋婢跺鍘遍梺鍝勬储閸斿本鏅堕鐐寸厽闁规儳顕幊鍕庨崶褝宸ラ摶鏍煃瑜滈崜鐔煎箖瑜斿畷銊╊敍濞戣鲸缍楅梻浣筋潐椤旀牠宕伴幒妤€纾婚柟鎯х摠婵挳鏌涢幇鐢靛帥婵☆偄鐗撳娲箹閻愭壆绀冮梺鎼炲劀閸涱厽鎲㈤梻鍌欑閹碱偊鎮ц箛娑樻瀬闁归棿鐒﹂崑鈩冪節闂堟侗鍎愰柣鎾存礋閺岀喖鎮滃Ο鐑╂嫻濡炪倕娴氬ú濉絪emove濠电姷鏁搁崑娑㈡偤閵娧冨灊鐎光偓閸曞灚鏅為梺鍛婃处閸嬧偓闁哄閰ｉ弻鏇＄疀鐎ｎ亖鍋撻弽顓炵柧妞ゆ帒瀚崐鍫曟煟閹邦喗鏆╅柟钘夊€块弻娑㈠Χ閸愩劉鍋撳┑瀣摕闁靛牆妫Σ褰掑箹閹碱厼鏋熸い銉焻tmousetrack濠电姷鏁告慨浼村垂濞差亜纾块柛蹇曨儠娴犲牓鏌熼梻瀵割槮闁?
        // mac fullscreen must show title bar
#ifdef Q_OS_OSX
        //setWindowFlags(windowFlags() & ~Qt::FramelessWindowHint);
#endif
        showToolForm(false);
        if (m_skin) {
            layout()->setContentsMargins(0, 0, 0, 0);
        }
        showFullScreen();

        // 闂傚倸鍊烽懗鍫曗€﹂崼銏″床闁割偁鍎辩粈澶屸偓鍏夊亾闁告洦鍓欓崜鐢告⒑缁洖澧茬紒瀣浮瀹曟垿骞囬悧鍫㈠幘闂佸憡绺块崕娲汲椤栫偞鐓曢悗锝庡亝鐏忎即鏌熷畡鐗堝櫤缂佹鍠栭、娑樷槈濮樺崬骞€婵犵數濮甸鏍窗濡ゅ啰绱﹂柛褎顨呴崹鍌氣攽閻樺疇澹橀柡瀣╃窔閺岀喖姊荤€电濡介梺鎼炲€曞ú顓㈠蓟閻旇　鍋撳☉娆樼劷闁活厼鐭傞弻鐔煎礂閻撳骸顫掗梺鍝勭焿缂嶄線銆侀弮鍫濆耿婵＄偑鍎抽崑銈夊蓟瀹ュ牜妾ㄩ梺绋跨箲閻╊垶骞冮悙鐑樻櫆闁芥ê顦介崵銈夋⒑閸涘﹣绶遍柛顭戝灠閳?
#ifdef Q_OS_WIN32
        ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
#endif
    }
}

bool VideoForm::isHost()
{
    if (!m_toolForm) {
        return false;
    }
    return m_toolForm->isHost();
}

void VideoForm::updateFPS(quint32 fps)
{
    //qDebug() << "FPS:" << fps;
    if (!m_fpsLabel) {
        return;
    }
    m_fpsLabel->setText(QString("FPS:%1").arg(fps));
}

void VideoForm::grabCursor(bool grab)
{
    if (!grab) {
        releaseGrabbedCursorState();
        centerCursorToVideoFrame();
        return;
    }

    hideLocalTextInputOverlay(false);
    m_cursorGrabbed = true;
    reloadRelativeLookInputConfig();

    QRect rc = getGrabCursorRect();
    MouseTap::getInstance()->enableMouseEventTap(rc, true);

    const bool enableRawInput = m_rawInputEnabled;
    setRawInputActive(enableRawInput);
}

void VideoForm::centerCursorToVideoFrame()
{
    if (!isVisible()) {
        return;
    }

    if (m_videoWidget && !m_videoWidget->rect().isEmpty()) {
        QCursor::setPos(m_videoWidget->mapToGlobal(m_videoWidget->rect().center()));
        return;
    }

    if (ui && ui->keepRatioWidget && !ui->keepRatioWidget->rect().isEmpty()) {
        QCursor::setPos(ui->keepRatioWidget->mapToGlobal(ui->keepRatioWidget->rect().center()));
    }
}

bool VideoForm::canUseLocalTextInput() const
{
    if (!m_localTextInputEnabled || !m_videoEnabled || !m_localTextInput || !m_videoWidget || m_cursorGrabbed || isKeymapEditorActive()) {
        return false;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return false;
    }

    return !device->isCurrentCustomKeymap();
}

void VideoForm::positionLocalTextInput()
{
    if (!m_localTextInput || !ui || !ui->keepRatioWidget) {
        return;
    }

    QRect anchorRect = m_videoWidget ? m_videoWidget->geometry() : QRect();
    if (!anchorRect.isValid() || anchorRect.isEmpty()) {
        anchorRect = ui->keepRatioWidget->rect();
    }

    if (!anchorRect.isValid() || anchorRect.isEmpty()) {
        return;
    }

    const int sideMargin = 16;
    const int bottomMargin = 24;
    const int height = 38;
    const int maxWidth = qMax(120, anchorRect.width() - sideMargin * 2);
    const int preferredWidth = qRound(anchorRect.width() * 0.65);
    const int width = qBound(qMin(180, maxWidth), preferredWidth, maxWidth);
    const int x = anchorRect.x() + qMax(0, (anchorRect.width() - width) / 2);
    const int y = anchorRect.y() + qMax(sideMargin, anchorRect.height() - height - bottomMargin);

    m_localTextInput->setGeometry(x, y, width, height);
    m_localTextInput->raise();
}

void VideoForm::showLocalTextInputOverlay()
{
    if (!canUseLocalTextInput()) {
        return;
    }

    if (m_localTextInput->isVisible()) {
        hideLocalTextInputOverlay(true);
        return;
    }

    positionLocalTextInput();
    m_localTextInput->clear();
    m_localTextInput->show();
    m_localTextInput->raise();
    m_localTextInput->setFocus(Qt::ShortcutFocusReason);
    m_localTextInput->selectAll();
}

void VideoForm::hideLocalTextInputOverlay(bool restoreVideoFocus, bool clearText)
{
    if (!m_localTextInput) {
        return;
    }

    if (clearText) {
        m_localTextInput->clear();
    }
    m_localTextInput->hide();

    if (restoreVideoFocus && m_videoWidget) {
        m_videoWidget->setFocus(Qt::OtherFocusReason);
    }
}

void VideoForm::submitLocalTextInputOverlay()
{
    if (!m_localTextInput) {
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        hideLocalTextInputOverlay(true);
        return;
    }

    QString text = m_localTextInput->text();
    if (!text.isEmpty()) {
        qInfo() << "Local text input submitted via clipboard paste:"
                << "serial=" << m_serial
                << "textLength=" << text.length();
        emit device->setDeviceClipboardText(text, true);
    }

    hideLocalTextInputOverlay(true);
}

void VideoForm::reloadViewControlSeparationConfig()
{
    const QString iniPath = resolveUserDataIniPath();
    QSettings settings(iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    const QString serial = m_serial.trimmed();
    m_videoEnabled = settings.value("common/VideoEnabled", true).toBool();
    m_lockDirectionIndex = settings.value("common/LockDirectionIndex", 0).toInt();
    m_videoCenterCropSize = 0;
    if (!serial.isEmpty()) {
        bool cropOk = false;
        const QVariant serialCropValue = settings.value(QString("%1/%2").arg(serial, "VideoCenterCropSize"));
        if (serialCropValue.isValid()) {
            m_videoCenterCropSize = serialCropValue.toInt(&cropOk);
            if (!cropOk || m_videoCenterCropSize <= 0) {
                m_videoCenterCropSize = 0;
            }
        }
    }
    m_controlMapToScreen = m_videoEnabled && m_videoCenterCropSize > 0;
    m_videoSessionFirstFrameLogged = false;
    resetOrientationProbeState();
    if (!m_videoEnabled || m_videoCenterCropSize <= 0 || m_lockDirectionIndex > 0) {
        m_pendingInitialOrientation = -1;
    }
    applyVideoCanvasLayout();

    qInfo() << "Video session config loaded:"
            << "serial=" << (serial.isEmpty() ? QString("common") : serial)
            << "videoEnabled=" << m_videoEnabled
            << "lockDirectionIndex=" << m_lockDirectionIndex
            << "cropSize=" << m_videoCenterCropSize
            << "controlMapToScreen=" << m_controlMapToScreen
            << "sessionCanvas=" << m_frameSize
            << "contentRect=" << m_contentRect;

    if (!m_videoEnabled) {
        stopOrientationPolling();
    } else {
        startOrientationPollingIfNeeded();
    }
}

void VideoForm::releaseGrabbedCursorState()
{
    m_cursorGrabbed = false;
    hideLocalTextInputOverlay(false);

    QRect rc = getGrabCursorRect();
    MouseTap::getInstance()->enableMouseEventTap(rc, false);
    setRawInputActive(false);

    while (QGuiApplication::overrideCursor()) {
        QGuiApplication::restoreOverrideCursor();
    }
}

void VideoForm::resetOrientationProbeState()
{
    m_orientationBaseUiSize = QSize();
    m_orientationBaseValue = -1;
    m_orientationBaseReady = false;
}

bool VideoForm::parseSurfaceOrientationFromText(const QString &text, int &orientationOut)
{
    return parseWindowDisplaysOrientation(text, orientationOut)
        || parseDisplayOrientation(text, orientationOut)
        || parseInputOrientation(text, orientationOut);
}

void VideoForm::resetOrientationProbeTask()
{
    if (m_orientationProbeStepTimer) {
        m_orientationProbeStepTimer->stop();
    }
    if (m_orientationProbeBudgetTimer) {
        m_orientationProbeBudgetTimer->stop();
    }

    if (m_orientationProbeProcess) {
        m_orientationProbeProcess->disconnect(this);
        if (m_orientationProbeProcess->state() != QProcess::NotRunning) {
            m_orientationProbeProcess->kill();
        }
        m_orientationProbeProcess->deleteLater();
        m_orientationProbeProcess.clear();
    }

    m_orientationProbeCurrentSource.clear();
    m_orientationProbeStepIndex = -1;
    m_orientationProbeBusy = false;
}

void VideoForm::initOrientationPoller()
{
    if (m_orientationPollTimer) {
        return;
    }

    m_orientationPollTimer = new QTimer(this);
    m_orientationPollTimer->setInterval(kOrientationPollIntervalMs);
    connect(m_orientationPollTimer, &QTimer::timeout, this, &VideoForm::probeOrientationAsync);

    m_orientationProbeStepTimer = new QTimer(this);
    m_orientationProbeStepTimer->setSingleShot(true);
    connect(m_orientationProbeStepTimer, &QTimer::timeout, this, &VideoForm::onOrientationProbeStepTimeout);

    m_orientationProbeBudgetTimer = new QTimer(this);
    m_orientationProbeBudgetTimer->setSingleShot(true);
    connect(m_orientationProbeBudgetTimer, &QTimer::timeout, this, &VideoForm::onOrientationProbeBudgetTimeout);
}

void VideoForm::startOrientationPollingIfNeeded()
{
    if (!m_videoEnabled || !m_frameSize.isValid() || m_serial.trimmed().isEmpty()) {
        return;
    }

    initOrientationPoller();
    if (!m_orientationPollTimer->isActive()) {
        m_orientationPollTimer->start();
        qInfo() << "Orientation polling enabled:"
                << "intervalMs=" << kOrientationPollIntervalMs
                << "serial=" << m_serial
                << "videoEnabled=" << m_videoEnabled
                << "cropSize=" << m_videoCenterCropSize;
    }

    if (!m_orientationProbeBusy) {
        probeOrientationAsync();
    }
}

void VideoForm::stopOrientationPolling()
{
    if (m_orientationPollTimer) {
        m_orientationPollTimer->stop();
    }

    resetOrientationProbeState();
    resetOrientationProbeTask();
}

void VideoForm::probeOrientationAsync()
{
    if (!m_videoEnabled || !m_frameSize.isValid() || m_serial.trimmed().isEmpty()) {
        return;
    }

    if (m_orientationProbeBusy) {
        return;
    }

    initOrientationPoller();
    resetOrientationProbeTask();

    m_orientationProbeBusy = true;
    m_orientationProbeStepIndex = 0;
    m_orientationProbeCurrentSource.clear();
    m_orientationProbeTotalElapsed.restart();
    if (m_orientationProbeBudgetTimer) {
        m_orientationProbeBudgetTimer->start(kOrientationProbeBudgetTimeoutMs);
    }
    startNextOrientationProbeStep();
}

void VideoForm::startNextOrientationProbeStep()
{
    if (!m_orientationProbeBusy) {
        return;
    }

    QStringList probeArgs;
    switch (m_orientationProbeStepIndex) {
    case 0:
        m_orientationProbeCurrentSource = QStringLiteral("window displays");
        probeArgs << "shell" << "dumpsys" << "window" << "displays";
        break;
    case 1:
        m_orientationProbeCurrentSource = QStringLiteral("display");
        probeArgs << "shell" << "dumpsys" << "display";
        break;
    case 2:
        m_orientationProbeCurrentSource = QStringLiteral("input");
        probeArgs << "shell" << "dumpsys" << "input";
        break;
    default:
        qInfo() << "Orientation probe finished without a usable result:"
                << "probeDurationMs=" << m_orientationProbeTotalElapsed.elapsed();
        resetOrientationProbeTask();
        return;
    }

    QString adbPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));
    if (adbPath.trimmed().isEmpty()) {
        adbPath = "adb";
    }

    const QString serial = m_serial.trimmed();
    QStringList args;
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args.append(probeArgs);

    QProcess *process = new QProcess(this);
    m_orientationProbeProcess = process;
    m_orientationProbeStepElapsed.restart();

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VideoForm::handleOrientationProbeFinished);
    connect(process, &QProcess::errorOccurred, this, &VideoForm::onOrientationProbeProcessError);

    if (m_orientationProbeStepTimer) {
        m_orientationProbeStepTimer->start(kOrientationProbeStepTimeoutMs);
    }

    process->start(adbPath, args);
}

void VideoForm::advanceOrientationProbeStep()
{
    if (!m_orientationProbeBusy) {
        return;
    }

    if (m_orientationProbeStepTimer) {
        m_orientationProbeStepTimer->stop();
    }

    if (m_orientationProbeProcess) {
        m_orientationProbeProcess->disconnect(this);
        if (m_orientationProbeProcess->state() != QProcess::NotRunning) {
            m_orientationProbeProcess->kill();
        }
        m_orientationProbeProcess->deleteLater();
        m_orientationProbeProcess.clear();
    }

    ++m_orientationProbeStepIndex;
    startNextOrientationProbeStep();
}

void VideoForm::onOrientationProbeStepTimeout()
{
    if (!m_orientationProbeBusy) {
        return;
    }

    qWarning() << "Orientation probe step timeout:"
               << "source=" << m_orientationProbeCurrentSource
               << "stepTimeoutMs=" << kOrientationProbeStepTimeoutMs
               << "probeDurationMs=" << m_orientationProbeTotalElapsed.elapsed();
    advanceOrientationProbeStep();
}

void VideoForm::onOrientationProbeBudgetTimeout()
{
    if (!m_orientationProbeBusy) {
        return;
    }

    qWarning() << "Orientation probe total budget timeout:"
               << "totalBudgetMs=" << kOrientationProbeBudgetTimeoutMs
               << "lastSource=" << m_orientationProbeCurrentSource;
    resetOrientationProbeTask();
}

void VideoForm::onOrientationProbeProcessError(QProcess::ProcessError error)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process || process != m_orientationProbeProcess) {
        return;
    }

    qWarning() << "Orientation probe step error:"
               << "source=" << m_orientationProbeCurrentSource
               << "error=" << error
               << "probeDurationMs=" << m_orientationProbeTotalElapsed.elapsed();
    advanceOrientationProbeStep();
}

void VideoForm::applyResolvedOrientation(int orientation)
{
    if (m_lockDirectionIndex > 0) {
        if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
            m_orientationBaseReady = true;
            m_orientationBaseValue = orientation;
            m_orientationBaseUiSize = m_frameSize;
        }
        return;
    }

    if (m_videoCenterCropSize > 0) {
        if (!m_frameSize.isValid() || m_frameSize.isEmpty()) {
            return;
        }

        if (m_pendingInitialOrientation >= 0) {
            qInfo() << "Orientation probe waiting for initial orientation hint:"
                    << "pendingInitialOrientation=" << m_pendingInitialOrientation
                    << "canvas=" << m_frameSize
                    << "orientation=" << orientation;
            return;
        }

        if (!m_orientationBaseReady) {
            m_orientationBaseReady = true;
            m_orientationBaseValue = orientation;
            m_orientationBaseUiSize = m_frameSize;
            qInfo() << "Orientation probe using fallback baseline:"
                    << "orientation=" << orientation
                    << "canvas=" << m_frameSize
                    << "cropSize=" << m_videoCenterCropSize;
            return;
        }

        const int previousOrientation = m_orientationBaseValue;
        const int delta = (orientation - previousOrientation + 4) % 4;
        if ((delta % 2) == 1 && m_frameSize.isValid() && !m_frameSize.isEmpty()) {
            const QSize oldCanvas = m_frameSize;
            const QSize targetCanvas = m_frameSize.transposed();
            if (targetCanvas.isValid() && targetCanvas != m_frameSize) {
                qInfo() << "Orientation probe transposed cropped canvas:"
                        << "oldCanvas=" << oldCanvas
                        << "newCanvas=" << targetCanvas
                        << "orientation=" << orientation
                        << "baseOrientation=" << previousOrientation
                        << "cropSize=" << m_videoCenterCropSize;
                updateShowSize(targetCanvas);
                applyVideoCanvasLayout();
            }
        }

        m_orientationBaseReady = true;
        m_orientationBaseValue = orientation;
        m_orientationBaseUiSize = m_frameSize;
        return;
    }

    if (!m_orientationBaseReady) {
        if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
            m_orientationBaseReady = true;
            m_orientationBaseValue = orientation;
            m_orientationBaseUiSize = m_frameSize;
        }
        return;
    }

    const int delta = (orientation - m_orientationBaseValue + 4) % 4;
    QSize targetSize = m_orientationBaseUiSize;
    if ((delta % 2) == 1) {
        targetSize.transpose();
    }
    if (targetSize.isValid() && targetSize != m_frameSize) {
        updateShowSize(targetSize);
    }
    if (m_frameSize.isValid() && !m_frameSize.isEmpty()) {
        m_orientationBaseReady = true;
        m_orientationBaseValue = orientation;
        m_orientationBaseUiSize = m_frameSize;
    }
}

void VideoForm::handleOrientationProbeFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process || process != m_orientationProbeProcess) {
        return;
    }

    if (m_orientationProbeStepTimer) {
        m_orientationProbeStepTimer->stop();
    }

    const QString output = QString::fromUtf8(process->readAllStandardOutput())
                           + QString::fromUtf8(process->readAllStandardError());
    int orientation = -1;
    const bool success = status == QProcess::NormalExit
        && exitCode == 0
        && parseSurfaceOrientationFromText(output, orientation);

    if (success) {
        qInfo() << "Orientation probe step result:"
                << "source=" << m_orientationProbeCurrentSource
                << "probeDurationMs=" << m_orientationProbeTotalElapsed.elapsed()
                << "orientation=" << orientation
                << "result=success";
        applyResolvedOrientation(orientation);
        resetOrientationProbeTask();
        return;
    }

    qInfo() << "Orientation probe step result:"
            << "source=" << m_orientationProbeCurrentSource
            << "probeDurationMs=" << m_orientationProbeTotalElapsed.elapsed()
            << "result=fallback";
    advanceOrientationProbeStep();
}

QSize VideoForm::eventFrameSize() const
{
    if (m_controlMapToScreen) {
        return QSize(65535, 65535);
    }

    if (m_videoWidget) {
        return m_videoWidget->frameSize();
    }

    return m_frameSize;
}

QSize VideoForm::eventShowSize() const
{
    if (m_videoWidget && !m_videoWidget->size().isEmpty()) {
        return m_videoWidget->size();
    }
    return size();
}

void VideoForm::loadVideoEnabledConfig()
{
    reloadViewControlSeparationConfig();
}
void VideoForm::updateNoVideoOverlay()
{
    if (!m_noVideoLabel || !ui || !ui->keepRatioWidget) {
        return;
    }

    m_noVideoLabel->setGeometry(ui->keepRatioWidget->rect());
    const bool showOverlay = !m_videoEnabled;
    m_noVideoLabel->setVisible(showOverlay);
    if (showOverlay) {
        m_noVideoLabel->raise();
    }
}

void VideoForm::initAiUdpReceiver()
{
    if (m_aiUdpSocket) {
        return;
    }

    m_aiUdpSocket = new QUdpSocket(this);
    const bool ok = m_aiUdpSocket->bind(QHostAddress::LocalHost, kAiUdpPort,
                                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
    if (!ok) {
        qWarning() << "AI UDP bind failed on port" << kAiUdpPort;
        return;
    }

    connect(m_aiUdpSocket, &QUdpSocket::readyRead, this, &VideoForm::onAiUdpReadyRead);
}

void VideoForm::onAiUdpReadyRead()
{
    if (!m_aiUdpSocket) {
        return;
    }

    while (m_aiUdpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_aiUdpSocket->pendingDatagramSize()));
        if (m_aiUdpSocket->readDatagram(datagram.data(), datagram.size()) <= 0) {
            continue;
        }
        if (datagram.size() < static_cast<int>(sizeof(AiDeltaPacketV1))) {
            continue;
        }

        AiDeltaPacketV1 packet;
        std::memcpy(&packet, datagram.constData(), sizeof(AiDeltaPacketV1));
        if (packet.magic != kAiDeltaMagic || packet.version != kAiDeltaVersion) {
            continue;
        }
        if (!std::isfinite(packet.aiDx) || !std::isfinite(packet.aiDy)) {
            continue;
        }

        m_aiRawInputAccumDelta.rx() += static_cast<qreal>(packet.aiDx);
        m_aiRawInputAccumDelta.ry() += static_cast<qreal>(packet.aiDy);
    }
}

void VideoForm::initRelativeLookConfigWatcher()
{
    m_relativeLookConfigWatcher = new QFileSystemWatcher(this);
    m_relativeLookConfigDebounceTimer = new QTimer(this);
    m_relativeLookConfigDebounceTimer->setSingleShot(true);

    connect(m_relativeLookConfigDebounceTimer, &QTimer::timeout, this, [this]() {
        ensureRelativeLookConfigWatchPath();
        reloadRelativeLookInputConfig();
    });

    connect(m_relativeLookConfigWatcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &) {
        ensureRelativeLookConfigWatchPath();
        if (m_relativeLookConfigDebounceTimer) {
            m_relativeLookConfigDebounceTimer->start(kRelativeLookConfigDebounceMs);
        }
    });

    connect(m_relativeLookConfigWatcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &) {
        ensureRelativeLookConfigWatchPath();
        if (m_relativeLookConfigDebounceTimer) {
            m_relativeLookConfigDebounceTimer->start(kRelativeLookConfigDebounceMs);
        }
    });

    ensureRelativeLookConfigWatchPath();
}

void VideoForm::ensureRelativeLookConfigWatchPath()
{
    if (!m_relativeLookConfigWatcher) {
        return;
    }

    const QString currentIniPath = resolveUserDataIniPath();
    if (m_relativeLookConfigPath != currentIniPath && !m_relativeLookConfigPath.isEmpty()) {
        m_relativeLookConfigWatcher->removePath(m_relativeLookConfigPath);
    }
    m_relativeLookConfigPath = currentIniPath;

    QFileInfo iniInfo(m_relativeLookConfigPath);
    const QString currentDirPath = iniInfo.absolutePath();
    if (m_relativeLookConfigDirPath != currentDirPath && !m_relativeLookConfigDirPath.isEmpty()) {
        m_relativeLookConfigWatcher->removePath(m_relativeLookConfigDirPath);
    }
    m_relativeLookConfigDirPath = currentDirPath;

    if (!m_relativeLookConfigDirPath.isEmpty() && !m_relativeLookConfigWatcher->directories().contains(m_relativeLookConfigDirPath)) {
        m_relativeLookConfigWatcher->addPath(m_relativeLookConfigDirPath);
    }

    if (iniInfo.exists() && iniInfo.isFile() && !m_relativeLookConfigWatcher->files().contains(m_relativeLookConfigPath)) {
        m_relativeLookConfigWatcher->addPath(m_relativeLookConfigPath);
    }
}

void VideoForm::reloadRelativeLookInputConfig()
{
    QString iniPath = resolveUserDataIniPath();
    QFileInfo fileInfo(iniPath);
    const qint64 modifiedMs = fileInfo.exists() ? fileInfo.lastModified().toMSecsSinceEpoch() : -1;
    if (m_relativeLookConfigLoaded && iniPath == m_relativeLookConfigPath && modifiedMs == m_relativeLookConfigLastModifiedMs) {
        return;
    }

    QSettings settings(iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    const QString serialKeyPrefix = m_serial.trimmed();
    const bool hasSerialSection = !serialKeyPrefix.isEmpty();
    const QString rawInputDeviceKey = hasSerialSection ? (serialKeyPrefix + "/RelativeLookRawInput") : QString();
    const QString sendHzDeviceKey = hasSerialSection ? (serialKeyPrefix + "/RelativeLookSendHz") : QString();
    const QString rawScaleDeviceKey = hasSerialSection ? (serialKeyPrefix + "/RelativeLookRawScale") : QString();
    const QString recoilDeviceKey = hasSerialSection ? (serialKeyPrefix + "/RelativeLookRecoilStrength") : QString();

    const bool useDeviceRawInput = hasSerialSection && settings.contains(rawInputDeviceKey);
    const bool useDeviceSendHz = hasSerialSection && settings.contains(sendHzDeviceKey);
    const bool useDeviceRawScale = hasSerialSection && settings.contains(rawScaleDeviceKey);
    const bool useDeviceRecoil = hasSerialSection && settings.contains(recoilDeviceKey);

    m_rawInputEnabled = useDeviceRawInput
        ? settings.value(rawInputDeviceKey).toBool()
        : settings.value("common/RelativeLookRawInput", true).toBool();

    int sendHz = useDeviceSendHz
        ? settings.value(sendHzDeviceKey).toInt()
        : settings.value("common/RelativeLookSendHz", 240).toInt();
    m_rawInputSendHz = qBound(kRawInputSendHzMin, sendHz, kRawInputSendHzMax);

    bool scaleOk = false;
    double scale = (useDeviceRawScale
        ? settings.value(rawScaleDeviceKey)
        : settings.value("common/RelativeLookRawScale", 12.0)).toDouble(&scaleOk);
    if (!scaleOk) {
        scale = 12.0;
    }
    m_rawInputScale = qBound(kRawInputScaleMin, scale, kRawInputScaleMax);

    bool recoilOk = false;
    double recoilStrength = (useDeviceRecoil
        ? settings.value(recoilDeviceKey)
        : settings.value("common/RelativeLookRecoilStrength", 0.0)).toDouble(&recoilOk);
    if (!recoilOk) {
        recoilStrength = 0.0;
    }
    if (recoilStrength < 0.0) {
        recoilStrength = 0.0;
    }
    m_recoilStrength = recoilStrength;

    m_relativeLookConfigLastModifiedMs = modifiedMs;
    m_relativeLookConfigLoaded = true;

    const bool shouldRawInputBeActive = m_cursorGrabbed && m_rawInputEnabled;
    if (m_rawInputActive != shouldRawInputBeActive) {
        setRawInputActive(shouldRawInputBeActive);
    } else if (m_rawInputActive && m_rawInputSendTimer) {
        m_rawInputSendTimer->start(qMax(1, qRound(1000.0 / m_rawInputSendHz)));
    }

    qInfo() << "RelativeLook input config loaded:"
            << "rawInput=" << m_rawInputEnabled
            << "sendHz=" << m_rawInputSendHz
            << "rawScale=" << m_rawInputScale
            << "recoil=" << m_recoilStrength
            << "source=" << (hasSerialSection ? serialKeyPrefix : "common");
}

void VideoForm::setRawInputActive(bool active)
{
#if defined(Q_OS_WIN32)
    if (m_rawInputActive == active) {
        return;
    }

    if (active) {
        RAWINPUTDEVICE rawInputDevice;
        rawInputDevice.usUsagePage = 0x01;
        rawInputDevice.usUsage = 0x02;
        rawInputDevice.dwFlags = 0;
        rawInputDevice.hwndTarget = reinterpret_cast<HWND>(winId());

        if (!RegisterRawInputDevices(&rawInputDevice, 1, sizeof(rawInputDevice))) {
            qWarning() << "RegisterRawInputDevices failed, fallback to mouse move events.";
            m_rawInputRegistered = false;
            m_rawInputActive = false;
            return;
        }

        m_rawInputRegistered = true;
        m_rawInputActive = true;
        m_leftButtonDown = false;
        m_rawInputAccumDelta = QPointF(0.0, 0.0);
        m_aiRawInputAccumDelta = QPointF(0.0, 0.0);
        m_rawInputVirtualPos = QPointF(0.0, 0.0);
        dispatchRawInputMouseMove(true);
        if (m_rawInputSendTimer) {
            m_rawInputSendTimer->start(qMax(1, qRound(1000.0 / m_rawInputSendHz)));
        }
    } else {
        if (m_rawInputSendTimer) {
            m_rawInputSendTimer->stop();
        }

        if (m_rawInputRegistered) {
            RAWINPUTDEVICE rawInputDevice;
            rawInputDevice.usUsagePage = 0x01;
            rawInputDevice.usUsage = 0x02;
            rawInputDevice.dwFlags = RIDEV_REMOVE;
            rawInputDevice.hwndTarget = nullptr;
            RegisterRawInputDevices(&rawInputDevice, 1, sizeof(rawInputDevice));
        }

        m_rawInputRegistered = false;
        m_rawInputActive = false;
        m_leftButtonDown = false;
        m_rawInputAccumDelta = QPointF(0.0, 0.0);
        m_aiRawInputAccumDelta = QPointF(0.0, 0.0);
    }
#else
    Q_UNUSED(active)
#endif
}

void VideoForm::dispatchRawInputMouseMove(bool forceSend)
{
#if defined(Q_OS_WIN32)
    if (!m_rawInputActive || !m_videoWidget) {
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }

    QPointF rawDelta = m_rawInputAccumDelta;
    m_rawInputAccumDelta = QPointF(0.0, 0.0);
    rawDelta += m_aiRawInputAccumDelta;
    m_aiRawInputAccumDelta = QPointF(0.0, 0.0);

    QPointF scaledDelta(rawDelta.x() * m_rawInputScale, rawDelta.y() * m_rawInputScale);
    const bool customKeymapActive = m_cursorGrabbed && device->isCurrentCustomKeymap();
    if (m_recoilStrength > 0.0 && m_leftButtonDown && customKeymapActive) {
        scaledDelta.ry() += m_recoilStrength;
    }
    if (!forceSend && qFuzzyIsNull(scaledDelta.x()) && qFuzzyIsNull(scaledDelta.y())) {
        return;
    }

    m_rawInputVirtualPos += scaledDelta;

    const QPointF globalSentinel(kRawSyntheticGlobalSentinel, kRawSyntheticGlobalSentinel);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QMouseEvent mouseEvent(QEvent::MouseMove, m_rawInputVirtualPos, globalSentinel,
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
#else
    QMouseEvent mouseEvent(QEvent::MouseMove, m_rawInputVirtualPos, globalSentinel,
                           Qt::NoButton, Qt::NoButton, Qt::NoModifier);
#endif
    emit device->mouseEvent(&mouseEvent, eventFrameSize(), eventShowSize());
#else
    Q_UNUSED(forceSend)
#endif
}

void VideoForm::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV, int linesizeY, int linesizeU, int linesizeV)
{
    updateRender(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void VideoForm::staysOnTop(bool top)
{
    bool needShow = false;
    if (isVisible()) {
        needShow = true;
    }
    setWindowFlag(Qt::WindowStaysOnTopHint, top);
    if (m_toolForm) {
        m_toolForm->setWindowFlag(Qt::WindowStaysOnTopHint, top);
    }
    if (needShow) {
        show();
    }
}

void VideoForm::mousePressEvent(QMouseEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::MiddleButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoHome();
            return;
        }
    }

    if (event->button() == Qt::RightButton) {
        if (device && !device->isCurrentCustomKeymap()) {
            device->postGoBack();
            return;
        }
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif

    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, eventFrameSize(), eventShowSize());

        // debug keymap pos
        if (event->button() == Qt::LeftButton) {
            qreal x = localPos.x() / m_videoWidget->size().width();
            qreal y = localPos.y() / m_videoWidget->size().height();
            QString posTip = QString(R"("pos": {"x": %1, "y": %2})").arg(x).arg(y);
            qInfo() << posTip.toStdString().c_str();
        }
    } else {
        if (event->button() == Qt::LeftButton) {
            m_dragPosition = globalPos.toPoint() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void VideoForm::mouseReleaseEvent(QMouseEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_dragPosition.isNull()) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        // local check
        QPointF local = m_videoWidget->mapFrom(this, localPos.toPoint());
        if (local.x() < 0) {
            local.setX(0);
        }
        if (local.x() > m_videoWidget->width()) {
            local.setX(m_videoWidget->width());
        }
        if (local.y() < 0) {
            local.setY(0);
        }
        if (local.y() > m_videoWidget->height()) {
            local.setY(m_videoWidget->height());
        }
        QMouseEvent newEvent(event->type(), local, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, eventFrameSize(), eventShowSize());
    } else {
        m_dragPosition = QPoint(0, 0);
    }
}

void VideoForm::mouseMoveEvent(QMouseEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (m_videoWidget->geometry().contains(event->pos())) {
        if (m_rawInputActive && m_cursorGrabbed) {
            return;
        }
        if (!device) {
            return;
        }
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, eventFrameSize(), eventShowSize());
    } else if (!m_dragPosition.isNull()) {
        if (event->buttons() & Qt::LeftButton) {
            move(globalPos.toPoint() - m_dragPosition);
            event->accept();
        }
    }
}

#if defined(Q_OS_WIN32)
bool VideoForm::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

    MSG *msg = static_cast<MSG *>(message);
    if (!m_rawInputActive || !m_cursorGrabbed || !msg || msg->message != WM_INPUT) {
        return QWidget::nativeEvent(eventType, message, result);
    }

    UINT size = 0;
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0
        || size == 0) {
        return QWidget::nativeEvent(eventType, message, result);
    }

    QByteArray buffer(static_cast<int>(size), 0);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg->lParam), RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        return QWidget::nativeEvent(eventType, message, result);
    }

    RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(buffer.data());
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        const USHORT buttonFlags = raw->data.mouse.usButtonFlags;
        if (buttonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
            m_leftButtonDown = true;
        }
        if (buttonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
            m_leftButtonDown = false;
        }
        m_rawInputAccumDelta.rx() += static_cast<qreal>(raw->data.mouse.lLastX);
        m_rawInputAccumDelta.ry() += static_cast<qreal>(raw->data.mouse.lLastY);
    }

    return QWidget::nativeEvent(eventType, message, result);
}
#endif

void VideoForm::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (event->button() == Qt::LeftButton && !m_videoWidget->geometry().contains(event->pos())) {
        if (!isMaximized()) {
            removeBlackRect();
        }
    }

    if (event->button() == Qt::RightButton && device && !device->isCurrentCustomKeymap()) {
        emit device->postBackOrScreenOn(event->type() == QEvent::MouseButtonPress);
    }

    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QPointF localPos = event->localPos();
        QPointF globalPos = event->globalPos();
#else
        QPointF localPos = event->position();
        QPointF globalPos = event->globalPosition();
#endif
        QPointF mappedPos = m_videoWidget->mapFrom(this, localPos.toPoint());
        QMouseEvent newEvent(event->type(), mappedPos, globalPos, event->button(), event->buttons(), event->modifiers());
        emit device->mouseEvent(&newEvent, eventFrameSize(), eventShowSize());
    }
}

void VideoForm::wheelEvent(QWheelEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    if (m_videoWidget->geometry().contains(event->position().toPoint())) {
        if (!device) {
            return;
        }
        QPointF pos = m_videoWidget->mapFrom(this, event->position().toPoint());
        QWheelEvent wheelEvent(
            pos, event->globalPosition(), event->pixelDelta(), event->angleDelta(), event->buttons(), event->modifiers(), event->phase(), event->inverted());
#else
    if (m_videoWidget->geometry().contains(event->pos())) {
        if (!device) {
            return;
        }
        QPointF pos = m_videoWidget->mapFrom(this, event->pos());

        QWheelEvent wheelEvent(
            pos, event->globalPosF(), event->pixelDelta(), event->angleDelta(), event->delta(), event->orientation(),
            event->buttons(), event->modifiers(), event->phase(), event->source(), event->inverted());
#endif
        emit device->wheelEvent(&wheelEvent, eventFrameSize(), eventShowSize());
    }
}

void VideoForm::keyPressEvent(QKeyEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    if (Qt::Key_Escape == event->key() && !event->isAutoRepeat() && isFullScreen()) {
        switchFullScreen();
    }

    emit device->keyEvent(event, eventFrameSize(), eventShowSize());
}

void VideoForm::keyReleaseEvent(QKeyEvent *event)
{
    if (isKeymapEditorActive()) {
        event->accept();
        return;
    }

    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    emit device->keyEvent(event, eventFrameSize(), eventShowSize());
}

void VideoForm::paintEvent(QPaintEvent *paint)
{
    Q_UNUSED(paint)
    QStyleOption opt;
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    opt.init(this);
#else
    opt.initFrom(this);
#endif
    QPainter p(this);
    style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
}

void VideoForm::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    applyTheme();
    ui->keepRatioWidget->relayoutNow();
    applyVideoCanvasLayout();
    positionLocalTextInput();
    positionKeymapEditorUi();
    if (m_pendingVideoWidgetReveal && m_videoWidget) {
        if (m_loadingWidget) {
            m_loadingWidget->close();
        }
        m_videoWidget->show();
        m_videoWidget->update();
        m_pendingVideoWidgetReveal = false;
    }
    if (!isFullScreen() && this->show_toolbar) {
        QTimer::singleShot(500, this, [this](){
            showToolForm(this->show_toolbar);
        });
    }
}

void VideoForm::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateNoVideoOverlay();
    ui->keepRatioWidget->relayoutNow();
    positionLocalTextInput();
    positionKeymapEditorUi();
    if (isVisible() && m_streamFrameSize.isValid()) {
        applyVideoCanvasLayout();
        if (m_pendingVideoWidgetReveal && m_videoWidget) {
            if (m_loadingWidget) {
                m_loadingWidget->close();
            }
            m_videoWidget->show();
            m_pendingVideoWidgetReveal = false;
        }
        if (m_videoWidget) {
            m_videoWidget->update();
        }
    }
    QSize goodSize = ui->keepRatioWidget->goodSize();
    if (goodSize.isEmpty()) {
        return;
    }
    QSize curSize = size();
    // 闂傚倸鍊搁崐鎼佸磹閸濄儮鍋撳鐓庡籍鐎规洘绻堝鎾閻樿櫕袣婵犳鍠栬墝闁稿鎮哾eoForm闂傚倷娴囬褏鎹㈤幇顔藉床闁圭増婢樼粻瑙勩亜閹拌泛鐦滈柡浣割儐閵囧嫰骞樼捄鐑樼亖闂佸磭绮濠氬焵椤掆偓缁犲秹宕曢柆宥嗗亱闁糕剝绋戦崒銊╂煙缂併垹鏋熼柛濠傜埣閻擃偊宕堕妸锕€鏆楃紓浣哄缁茶法妲愰幒妤€惟鐟滃酣宕曢—鐜RatioWidget good size
    if (m_widthHeightRatio > 1.0f) {
        // hor
        if (curSize.height() <= goodSize.height()) {
            setMinimumHeight(goodSize.height());
        } else {
            setMinimumHeight(0);
        }
    } else {
        // ver
        if (curSize.width() <= goodSize.width()) {
            setMinimumWidth(goodSize.width());
        } else {
            setMinimumWidth(0);
        }
    }
}

void VideoForm::closeEvent(QCloseEvent *event)
{
    Q_UNUSED(event)
    shutdownKeymapEditor(false);
    releaseGrabbedCursorState();
    stopOrientationPolling();
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    Config::getInstance().setRect(device->getSerial(), geometry());
    device->disconnectDevice();
}

void VideoForm::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void VideoForm::dragMoveEvent(QDragMoveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dragLeaveEvent(QDragLeaveEvent *event)
{
    Q_UNUSED(event)
}

void VideoForm::dropEvent(QDropEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (!device) {
        return;
    }
    const QMimeData *qm = event->mimeData();
    QList<QUrl> urls = qm->urls();

    for (const QUrl &url : urls) {
        QString file = url.toLocalFile();
        QFileInfo fileInfo(file);

        if (!fileInfo.exists()) {
            QMessageBox::warning(this, "QtScrcpy", tr("file does not exist"), QMessageBox::Ok);
            continue;
        }

        if (fileInfo.isFile() && fileInfo.suffix() == "apk") {
            emit device->installApkRequest(file);
            continue;
        }
        emit device->pushFileRequest(file, Config::getInstance().getPushFilePath() + fileInfo.fileName());
    }
}






