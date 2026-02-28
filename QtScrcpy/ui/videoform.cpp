// #include <QDesktopWidget>
#include <QCursor>
#include <QCoreApplication>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QRegularExpression>
#include <QHostAddress>
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

#if defined(Q_OS_WIN32)
#include <Windows.h>
#endif

#include "config.h"
#include "iconhelper.h"
#include "qyuvopenglwidget.h"
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
constexpr int kRelativeLookConfigDebounceMs = 120;
constexpr quint32 kAiDeltaMagic = 0x31444941U; // "AID1" little-endian
constexpr quint16 kAiDeltaVersion = 1;
constexpr quint16 kAiUdpPort = 12345;

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
}

VideoForm::~VideoForm()
{
    stopOrientationPolling();
    setRawInputActive(false);
    delete ui;
}

void VideoForm::initUI()
{
    loadVideoEnabledConfig();

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
    ui->keepRatioWidget->setWidget(m_videoWidget);
    ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

    m_noVideoLabel = new QLabel(ui->keepRatioWidget);
    m_noVideoLabel->setAlignment(Qt::AlignCenter);
    m_noVideoLabel->setText(tr("Pure Control Mode (Video Disabled)\nControl channel is active."));
    m_noVideoLabel->setStyleSheet("QLabel { color: #D0D0D0; background: #111111; font-size: 16px; }");
    m_noVideoLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_noVideoLabel->setFocusPolicy(Qt::NoFocus);
    m_noVideoLabel->hide();

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
    if (m_videoWidget->isHidden()) {
        if (m_loadingWidget) {
            m_loadingWidget->close();
        }
        m_videoWidget->show();
    }

    m_streamFrameSize = QSize(width, height);
    if (!m_controlMapToScreen || !m_frameSize.isValid()) {
        updateShowSize(m_streamFrameSize);
    }
    m_videoWidget->setFrameSize(m_streamFrameSize);
    m_videoWidget->updateTextures(dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
    updateNoVideoOverlay();
}
void VideoForm::setSerial(const QString &serial)
{
    m_serial = serial;
    reloadViewControlSeparationConfig();
    startOrientationPollingIfNeeded();
    updateNoVideoOverlay();
}
void VideoForm::showToolForm(bool show)
{
    if (!m_toolForm) {
        m_toolForm = new ToolForm(this, ToolForm::AP_OUTSIDE_RIGHT);
        m_toolForm->setSerial(m_serial);
    }
    m_toolForm->move(pos().x() + geometry().width(), pos().y() + 30);
    m_toolForm->setVisible(show);
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
    QShortcut *shortcut = nullptr;

    // switchFullScreen
    shortcut = new QShortcut(QKeySequence("Ctrl+f"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        switchFullScreen();
    });

    // resizeSquare
    shortcut = new QShortcut(QKeySequence("Ctrl+g"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { resizeSquare(); });

    // removeBlackRect
    shortcut = new QShortcut(QKeySequence("Ctrl+w"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() { removeBlackRect(); });

    // postGoHome
    shortcut = new QShortcut(QKeySequence("Ctrl+h"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoHome();
    });

    // postGoBack
    shortcut = new QShortcut(QKeySequence("Ctrl+b"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoBack();
    });

    // postAppSwitch
    shortcut = new QShortcut(QKeySequence("Ctrl+s"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postAppSwitch();
    });

    // postGoMenu
    shortcut = new QShortcut(QKeySequence("Ctrl+m"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        device->postGoMenu();
    });

    // postVolumeUp
    shortcut = new QShortcut(QKeySequence("Ctrl+up"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeUp();
    });

    // postVolumeDown
    shortcut = new QShortcut(QKeySequence("Ctrl+down"), this);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postVolumeDown();
    });

    // postPower
    shortcut = new QShortcut(QKeySequence("Ctrl+p"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postPower();
    });

    shortcut = new QShortcut(QKeySequence("Ctrl+o"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDisplayPower(false);
    });

    // expandNotificationPanel
    shortcut = new QShortcut(QKeySequence("Ctrl+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->expandNotificationPanel();
    });

    // collapsePanel
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+n"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->collapsePanel();
    });

    // copy
    shortcut = new QShortcut(QKeySequence("Ctrl+c"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCopy();
    });

    // cut
    shortcut = new QShortcut(QKeySequence("Ctrl+x"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->postCut();
    });

    // clipboardPaste
    shortcut = new QShortcut(QKeySequence("Ctrl+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->setDeviceClipboard();
    });

    // setDeviceClipboard
    shortcut = new QShortcut(QKeySequence("Ctrl+Shift+v"), this);
    shortcut->setAutoRepeat(false);
    connect(shortcut, &QShortcut::activated, this, [this]() {
        auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
        if (!device) {
            return;
        }
        emit device->clipboardPaste();
    });
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
        if (m_videoWidget && newSize.isValid()) {
            m_videoWidget->setFrameSize(newSize);
        }

        m_widthHeightRatio = 1.0f * newSize.width() / newSize.height();
        ui->keepRatioWidget->setWidthHeightRatio(m_widthHeightRatio);

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
        }
    }
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
    m_cursorGrabbed = grab;
    reloadRelativeLookInputConfig();

    QRect rc = getGrabCursorRect();
    MouseTap::getInstance()->enableMouseEventTap(rc, grab);

    const bool enableRawInput = grab && m_rawInputEnabled;
    setRawInputActive(enableRawInput);

    if (!grab) {
        centerCursorToVideoFrame();
    }
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

void VideoForm::reloadViewControlSeparationConfig()
{
    const QString iniPath = resolveUserDataIniPath();
    QSettings settings(iniPath, QSettings::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    settings.setIniCodec("UTF-8");
#endif

    const QString serial = m_serial.trimmed();
    auto readBoolWithSerialOverride = [&](const QString &key, bool defaultValue) -> bool {
        if (!serial.isEmpty()) {
            const QString serialPath = QString("%1/%2").arg(serial).arg(key);
            const QVariant serialValue = settings.value(serialPath);
            if (serialValue.isValid()) {
                return serialValue.toBool();
            }
        }
        return settings.value(QString("common/%1").arg(key), defaultValue).toBool();
    };
    auto readIntWithSerialOverride = [&](const QString &key, int defaultValue) -> int {
        bool ok = false;
        if (!serial.isEmpty()) {
            const QString serialPath = QString("%1/%2").arg(serial).arg(key);
            const QVariant serialValue = settings.value(serialPath);
            if (serialValue.isValid()) {
                const int parsed = serialValue.toInt(&ok);
                if (ok) {
                    return parsed;
                }
            }
        }
        const int parsed = settings.value(QString("common/%1").arg(key), defaultValue).toInt(&ok);
        return ok ? parsed : defaultValue;
    };

    m_videoEnabled = settings.value("common/VideoEnabled", true).toBool();
    const int cropSize = readIntWithSerialOverride("VideoCenterCropSize", 0);
    const bool mapToScreen = readBoolWithSerialOverride("VideoCenterCropMapToScreen", false);
    m_controlMapToScreen = m_videoEnabled && cropSize > 0 && mapToScreen;

    if (!m_controlMapToScreen) {
        stopOrientationPolling();
    } else {
        startOrientationPollingIfNeeded();
    }
}

bool VideoForm::parseSurfaceOrientationFromText(const QString &text, int &orientationOut)
{
    QRegularExpression surfaceOrientationRe(R"(SurfaceOrientation\s*:\s*([0-3]))",
                                            QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch surfaceMatch = surfaceOrientationRe.match(text);
    if (surfaceMatch.hasMatch()) {
        bool ok = false;
        const int value = surfaceMatch.captured(1).toInt(&ok);
        if (ok && value >= 0 && value <= 3) {
            orientationOut = value;
            return true;
        }
    }

    QRegularExpression fallbackRe(R"(orientation\s*[=:]\s*([0-3]))",
                                  QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch fallbackMatch = fallbackRe.match(text);
    if (fallbackMatch.hasMatch()) {
        bool ok = false;
        const int value = fallbackMatch.captured(1).toInt(&ok);
        if (ok && value >= 0 && value <= 3) {
            orientationOut = value;
            return true;
        }
    }

    return false;
}

void VideoForm::initOrientationPoller()
{
    if (m_orientationPollTimer) {
        return;
    }

    m_orientationPollTimer = new QTimer(this);
    m_orientationPollTimer->setInterval(2000);
    connect(m_orientationPollTimer, &QTimer::timeout, this, &VideoForm::probeOrientationAsync);
}

void VideoForm::startOrientationPollingIfNeeded()
{
    if (!m_controlMapToScreen || !m_frameSize.isValid() || m_serial.trimmed().isEmpty()) {
        return;
    }

    initOrientationPoller();
    if (!m_orientationPollTimer->isActive()) {
        m_orientationPollTimer->start();
    }

    if (!m_orientationProbeProcess) {
        probeOrientationAsync();
    }
}

void VideoForm::stopOrientationPolling()
{
    if (m_orientationPollTimer) {
        m_orientationPollTimer->stop();
    }

    m_orientationBaseUiSize = QSize();
    m_orientationBaseValue = -1;
    m_orientationBaseReady = false;

    if (m_orientationProbeProcess) {
        if (m_orientationProbeProcess->state() != QProcess::NotRunning) {
            m_orientationProbeProcess->terminate();
            if (!m_orientationProbeProcess->waitForFinished(200)) {
                m_orientationProbeProcess->kill();
                m_orientationProbeProcess->waitForFinished(200);
            }
        }
        m_orientationProbeProcess->deleteLater();
        m_orientationProbeProcess.clear();
    }
}

void VideoForm::probeOrientationAsync()
{
    if (!m_controlMapToScreen || !m_frameSize.isValid() || m_serial.trimmed().isEmpty()) {
        return;
    }

    if (m_orientationProbeProcess) {
        if (m_orientationProbeProcess->state() != QProcess::NotRunning) {
            return;
        }
        m_orientationProbeProcess->deleteLater();
        m_orientationProbeProcess.clear();
    }

    QString adbPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_ADB_PATH"));
    if (adbPath.trimmed().isEmpty()) {
        adbPath = "adb";
    }

    QStringList args;
    const QString serial = m_serial.trimmed();
    if (!serial.isEmpty()) {
        args << "-s" << serial;
    }
    args << "shell" << "dumpsys" << "input";

    QProcess *process = new QProcess(this);
    m_orientationProbeProcess = process;

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &VideoForm::handleOrientationProbeFinished);

    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        if (m_orientationProbeProcess == process) {
            m_orientationProbeProcess.clear();
        }
        process->deleteLater();
    });

    process->start(adbPath, args);
}

void VideoForm::handleOrientationProbeFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = qobject_cast<QProcess *>(sender());
    if (!process) {
        return;
    }

    const QString output = QString::fromUtf8(process->readAllStandardOutput())
                           + QString::fromUtf8(process->readAllStandardError());

    if (status == QProcess::NormalExit && exitCode == 0) {
        int orientation = 0;
        if (parseSurfaceOrientationFromText(output, orientation)) {
            if (!m_orientationBaseReady) {
                m_orientationBaseReady = true;
                m_orientationBaseValue = orientation;
                m_orientationBaseUiSize = m_frameSize;
            } else {
                const int delta = (orientation - m_orientationBaseValue + 4) % 4;
                QSize targetSize = m_orientationBaseUiSize;
                if ((delta % 2) == 1) {
                    targetSize.transpose();
                }
                if (targetSize.isValid() && targetSize != m_frameSize) {
                    updateShowSize(targetSize);
                }
            }
        }
    }

    if (m_orientationProbeProcess == process) {
        m_orientationProbeProcess.clear();
    }
    process->deleteLater();
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

    reloadViewControlSeparationConfig();
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
    Q_UNUSED(event)
    if (!isFullScreen() && this->show_toolbar) {
        QTimer::singleShot(500, this, [this](){
            showToolForm(this->show_toolbar);
        });
    }
}

void VideoForm::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event)
    updateNoVideoOverlay();
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






