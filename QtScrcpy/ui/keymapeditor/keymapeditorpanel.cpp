#include "keymapeditorpanel.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "keymapkeycodec.h"
#include "thememanager.h"

#ifdef Q_OS_WIN32
#include "../../util/winutils.h"
#include <Windows.h>
#endif

namespace {
QString nodeTypeLabel(KeymapEditorDocument::NodeType type)
{
    switch (type) {
    case KeymapEditorDocument::NodeMouseMove:
        return QStringLiteral("Mouse Look");
    case KeymapEditorDocument::NodeClick:
        return QStringLiteral("Single Click");
    case KeymapEditorDocument::NodeSteerWheel:
        return QStringLiteral("Steer Wheel");
    case KeymapEditorDocument::NodeDrag:
        return QStringLiteral("Drag");
    case KeymapEditorDocument::NodeAndroidKey:
        return QStringLiteral("Android Key");
    default:
        return QStringLiteral("Unknown");
    }
}

QString bindingDisplayToolTip(const QString &bindingName)
{
    return QObject::tr("%1显示当前已绑定的按键或鼠标键，不能直接输入；请使用右侧“Record”重新录制。").arg(bindingName);
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

QString shortcutDisplayText(const QKeySequence &shortcut)
{
    return normalizeSingleShortcut(shortcut).toString(QKeySequence::NativeText).trimmed();
}

QString recordButtonToolTip(const QString &bindingName, bool listening, const QString &closeShortcutText)
{
    if (!listening) {
        return QObject::tr("Start recording %1. The next key or mouse button input will be written into this binding.")
            .arg(bindingName);
    }

    QString toolTip = QObject::tr("Listening for %1. Press any key or mouse button to record it.")
        .arg(bindingName);
    if (closeShortcutText.isEmpty()) {
        toolTip += QObject::tr("\nNo editor-close shortcut is currently configured.");
    } else {
        toolTip += QObject::tr("\nPress %1 to cancel and close the editor.").arg(closeShortcutText);
    }
    return toolTip;
}

bool keyEventMatchesShortcut(const QKeyEvent *keyEvent, const QKeySequence &shortcut)
{
    if (!keyEvent) {
        return false;
    }

    const QKeySequence normalized = normalizeSingleShortcut(shortcut);
    if (normalized.isEmpty()) {
        return false;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QKeySequence(keyEvent->keyCombination()) == normalized;
#else
    return QKeySequence(keyEvent->key() | keyEvent->modifiers()) == normalized;
#endif
}
}

KeymapEditorPanel::KeymapEditorPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("keymapEditorPanel"));
    setWindowFlags(windowFlags() | Qt::Tool | Qt::WindowTitleHint | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::NonModal);
    setMinimumSize(320, 320);
    resize(340, 520);
    setWindowTitle(QStringLiteral("Keymap Editor"));
    setFocusPolicy(Qt::StrongFocus);

    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setText(QStringLiteral("Keymap Editor"));
    rootLayout->addWidget(m_titleLabel);

    m_nodeList = new QListWidget(this);
    m_nodeList->setToolTip(tr("显示当前脚本里的所有节点。点击列表项可切换右侧属性编辑对象。"));
    rootLayout->addWidget(m_nodeList, 1);

    QHBoxLayout *addLayout = new QHBoxLayout();
    m_addTypeBox = new QComboBox(this);
    m_addTypeBox->addItem(nodeTypeLabel(KeymapEditorDocument::NodeClick), KeymapEditorDocument::NodeClick);
    m_addTypeBox->addItem(nodeTypeLabel(KeymapEditorDocument::NodeDrag), KeymapEditorDocument::NodeDrag);
    m_addTypeBox->addItem(nodeTypeLabel(KeymapEditorDocument::NodeSteerWheel), KeymapEditorDocument::NodeSteerWheel);
    m_addTypeBox->addItem(nodeTypeLabel(KeymapEditorDocument::NodeAndroidKey), KeymapEditorDocument::NodeAndroidKey);
    m_addTypeBox->addItem(nodeTypeLabel(KeymapEditorDocument::NodeMouseMove), KeymapEditorDocument::NodeMouseMove);
    m_addTypeBox->setToolTip(tr("选择要新增的节点类型。新增节点会出现在画面中心附近。"));
    m_addNodeBtn = new QPushButton(QStringLiteral("Add"), this);
    m_addNodeBtn->setToolTip(tr("按左侧类型新增一个节点，并自动选中新节点。"));
    addLayout->addWidget(m_addTypeBox, 1);
    addLayout->addWidget(m_addNodeBtn);
    rootLayout->addLayout(addLayout);

    m_deleteNodeBtn = new QPushButton(QStringLiteral("Delete Node"), this);
    m_deleteNodeBtn->setToolTip(tr("删除当前选中的节点。只读节点或不可删除节点不会允许执行。"));
    rootLayout->addWidget(m_deleteNodeBtn);

    QFrame *separator = new QFrame(this);
    separator->setProperty("themeSeparator", true);
    separator->setFrameShape(QFrame::HLine);
    rootLayout->addWidget(separator);

    QFormLayout *formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignLeft);
    formLayout->setFormAlignment(Qt::AlignTop);

    m_commentEdit = new QLineEdit(this);
    m_commentEdit->setToolTip(tr("编辑当前节点的备注说明。只在该节点支持注释且不是只读时可修改。"));
    formLayout->addRow(QStringLiteral("Comment"), m_commentEdit);

    m_alwaysActiveCheck = new QCheckBox(QStringLiteral("Always Active"), this);
    m_alwaysActiveCheck->setToolTip(tr("让当前节点始终保持激活，不受常规切换状态影响。只有支持该属性的节点才会显示。"));
    formLayout->addRow(QString(), m_alwaysActiveCheck);

    m_switchMapCheck = new QCheckBox(QStringLiteral("Switch Map"), this);
    m_switchMapCheck->setToolTip(tr("把当前节点作为切换映射用的开关。只有支持该属性的节点才会显示。"));
    formLayout->addRow(QString(), m_switchMapCheck);

    auto makeRecordRow = [this](QLineEdit *&edit, QPushButton *&button, const QString &buttonText) -> QWidget * {
        QWidget *rowWidget = new QWidget(this);
        QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(6);
        edit = new QLineEdit(rowWidget);
        edit->setReadOnly(true);
        button = new QPushButton(buttonText, rowWidget);
        rowLayout->addWidget(edit, 1);
        rowLayout->addWidget(button);
        return rowWidget;
    };

    formLayout->addRow(QStringLiteral("Primary Key"), makeRecordRow(m_primaryKeyEdit, m_primaryRecordBtn, QStringLiteral("Record")));
    formLayout->addRow(QStringLiteral("Left Key"), makeRecordRow(m_leftKeyEdit, m_leftRecordBtn, QStringLiteral("Record")));
    formLayout->addRow(QStringLiteral("Right Key"), makeRecordRow(m_rightKeyEdit, m_rightRecordBtn, QStringLiteral("Record")));
    formLayout->addRow(QStringLiteral("Up Key"), makeRecordRow(m_upKeyEdit, m_upRecordBtn, QStringLiteral("Record")));
    formLayout->addRow(QStringLiteral("Down Key"), makeRecordRow(m_downKeyEdit, m_downRecordBtn, QStringLiteral("Record")));
    m_primaryKeyEdit->setToolTip(bindingDisplayToolTip(tr("主键位")));
    m_leftKeyEdit->setToolTip(bindingDisplayToolTip(tr("左方向键位")));
    m_rightKeyEdit->setToolTip(bindingDisplayToolTip(tr("右方向键位")));
    m_upKeyEdit->setToolTip(bindingDisplayToolTip(tr("上方向键位")));
    m_downKeyEdit->setToolTip(bindingDisplayToolTip(tr("下方向键位")));
    m_primaryRecordBtn->setToolTip(recordButtonToolTip(tr("主键位"), false, QString()));
    m_leftRecordBtn->setToolTip(recordButtonToolTip(tr("左方向键位"), false, QString()));
    m_rightRecordBtn->setToolTip(recordButtonToolTip(tr("右方向键位"), false, QString()));
    m_upRecordBtn->setToolTip(recordButtonToolTip(tr("上方向键位"), false, QString()));
    m_downRecordBtn->setToolTip(recordButtonToolTip(tr("下方向键位"), false, QString()));

    m_androidKeySpin = new QSpinBox(this);
    m_androidKeySpin->setRange(0, 500);
    m_androidKeySpin->setToolTip(tr("设置当前节点要发送的 Android KeyCode 数值。只有 Android Key 节点才会显示。"));
    formLayout->addRow(QStringLiteral("Android Key"), m_androidKeySpin);

    m_primaryPosLabel = new QLabel(this);
    m_secondaryPosLabel = new QLabel(this);
    m_smallEyesPosLabel = new QLabel(this);
    formLayout->addRow(QStringLiteral("Primary Pos"), m_primaryPosLabel);
    formLayout->addRow(QStringLiteral("Secondary Pos"), m_secondaryPosLabel);
    formLayout->addRow(QStringLiteral("SmallEyes Pos"), m_smallEyesPosLabel);

    rootLayout->addLayout(formLayout);

    m_recordingHintLabel = new QLabel(this);
    m_recordingHintLabel->hide();
    rootLayout->addWidget(m_recordingHintLabel);

    QHBoxLayout *bottomLayout = new QHBoxLayout();
    m_saveBtn = new QPushButton(QStringLiteral("Save && Apply"), this);
    m_discardBtn = new QPushButton(QStringLiteral("Discard"), this);
    m_saveBtn->setToolTip(tr("保存当前脚本修改并立即重新应用到当前视频窗口。"));
    m_discardBtn->setToolTip(tr("放弃尚未保存的修改，恢复到上次已应用的脚本内容。"));
    bottomLayout->addWidget(m_saveBtn, 1);
    bottomLayout->addWidget(m_discardBtn, 1);
    rootLayout->addLayout(bottomLayout);

    connect(m_nodeList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || !m_document || row >= m_document->nodeInfos().size()) {
            m_selectedNodeId = -1;
            emit nodeSelected(-1);
            refreshSelection();
            return;
        }
        const QVector<KeymapEditorDocument::NodeInfo> nodes = m_document->nodeInfos();
        m_selectedNodeId = nodes.at(row).id;
        emit nodeSelected(m_selectedNodeId);
        refreshSelection();
    });

    connect(m_addNodeBtn, &QPushButton::clicked, this, [this]() {
        if (!m_document) {
            return;
        }
        const KeymapEditorDocument::NodeType type = static_cast<KeymapEditorDocument::NodeType>(m_addTypeBox->currentData().toInt());
        const int nodeId = m_document->createNode(type, QPointF(0.5, 0.5));
        if (nodeId >= 0) {
            setSelectedNodeId(nodeId);
        }
    });
    connect(m_deleteNodeBtn, &QPushButton::clicked, this, [this]() {
        if (!m_document || m_selectedNodeId < 0) {
            return;
        }
        if (m_document->deleteNode(m_selectedNodeId)) {
            m_selectedNodeId = -1;
            emit nodeSelected(-1);
        }
    });
    connect(m_commentEdit, &QLineEdit::textEdited, this, [this](const QString &text) {
        if (m_document && m_selectedNodeId >= 0) {
            m_document->setComment(m_selectedNodeId, text);
        }
    });
    connect(m_alwaysActiveCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_document && m_selectedNodeId >= 0) {
            m_document->setAlwaysActive(m_selectedNodeId, checked);
        }
    });
    connect(m_switchMapCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_document && m_selectedNodeId >= 0) {
            m_document->setSwitchMap(m_selectedNodeId, checked);
        }
    });
    connect(m_androidKeySpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, [this](int value) {
        if (m_document && m_selectedNodeId >= 0) {
            m_document->setAndroidKey(m_selectedNodeId, value);
        }
    });

    connect(m_primaryRecordBtn, &QPushButton::clicked, this, [this]() {
        setRecordingState(true, m_selectedNodeId, KeymapEditorDocument::BindingPrimary, QStringLiteral("Press key or mouse button for primary binding"));
    });
    connect(m_leftRecordBtn, &QPushButton::clicked, this, [this]() {
        setRecordingState(true, m_selectedNodeId, KeymapEditorDocument::BindingSteerLeft, QStringLiteral("Press key or mouse button for left binding"));
    });
    connect(m_rightRecordBtn, &QPushButton::clicked, this, [this]() {
        setRecordingState(true, m_selectedNodeId, KeymapEditorDocument::BindingSteerRight, QStringLiteral("Press key or mouse button for right binding"));
    });
    connect(m_upRecordBtn, &QPushButton::clicked, this, [this]() {
        setRecordingState(true, m_selectedNodeId, KeymapEditorDocument::BindingSteerUp, QStringLiteral("Press key or mouse button for up binding"));
    });
    connect(m_downRecordBtn, &QPushButton::clicked, this, [this]() {
        setRecordingState(true, m_selectedNodeId, KeymapEditorDocument::BindingSteerDown, QStringLiteral("Press key or mouse button for down binding"));
    });
    connect(m_saveBtn, &QPushButton::clicked, this, &KeymapEditorPanel::saveRequested);
    connect(m_discardBtn, &QPushButton::clicked, this, &KeymapEditorPanel::discardRequested);

    setCloseShortcut(m_closeKeySequence);

    connect(&ThemeManager::getInstance(), &ThemeManager::themeChanged, this, [this]() {
        applyTheme();
    });
    applyTheme();
    refreshSelection();
}

KeymapEditorPanel::~KeymapEditorPanel()
{
    setRecordingState(false);
}

void KeymapEditorPanel::setDocument(KeymapEditorDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(m_document, &KeymapEditorDocument::documentReset, this, &KeymapEditorPanel::rebuildNodeList);
        connect(m_document, &KeymapEditorDocument::nodeListChanged, this, &KeymapEditorPanel::rebuildNodeList);
        connect(m_document, &KeymapEditorDocument::nodeChanged, this, [this](int nodeId) {
            if (nodeId == m_selectedNodeId) {
                refreshSelection();
            }
            rebuildNodeList();
        });
        connect(m_document, &KeymapEditorDocument::dirtyChanged, this, [this](bool dirty) {
            m_saveBtn->setText(dirty ? QStringLiteral("Save && Apply *") : QStringLiteral("Save && Apply"));
        });
    }
    rebuildNodeList();
}

void KeymapEditorPanel::setSelectedNodeId(int nodeId)
{
    if (m_selectedNodeId == nodeId) {
        return;
    }
    m_selectedNodeId = nodeId;
    rebuildNodeList();
    refreshSelection();
}

int KeymapEditorPanel::selectedNodeId() const
{
    return m_selectedNodeId;
}

void KeymapEditorPanel::setScriptDisplayName(const QString &displayName)
{
    const QString title = displayName.isEmpty() ? QStringLiteral("Keymap Editor") : QStringLiteral("Keymap Editor - %1").arg(displayName);
    m_titleLabel->setText(title);
    setWindowTitle(title);
}

void KeymapEditorPanel::setCloseShortcut(const QKeySequence &shortcut)
{
    m_closeKeySequence = normalizeSingleShortcut(shortcut);

    if (m_closeShortcut) {
        delete m_closeShortcut;
        m_closeShortcut = nullptr;
    }

    if (!m_closeKeySequence.isEmpty()) {
        m_closeShortcut = new QShortcut(m_closeKeySequence, this);
        m_closeShortcut->setAutoRepeat(false);
        connect(m_closeShortcut, &QShortcut::activated, this, [this]() {
            setRecordingState(false);
            emit closeRequested();
        });
    }

    updateRecordingButtons();
}

bool KeymapEditorPanel::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched)
    if (!m_recordingActive || m_selectedNodeId < 0) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (matchesCloseShortcut(keyEvent)) {
            event->accept();
            return false;
        }
        event->accept();
        return true;
    }
    case QEvent::KeyPress: {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (matchesCloseShortcut(keyEvent)) {
            setRecordingState(false);
            emit closeRequested();
            event->accept();
            return true;
        }
        if (keyEvent->isAutoRepeat()) {
            event->accept();
            return true;
        }
        const QString jsonKey = KeymapKeyCodec::encodeKey(keyEvent->key());
        if (!jsonKey.isEmpty()) {
            applyRecordedJsonKey(jsonKey);
        }
        event->accept();
        return true;
    }
    case QEvent::KeyRelease:
        event->accept();
        return true;
    case QEvent::MouseButtonPress: {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
        const QString jsonKey = KeymapKeyCodec::encodeMouseButton(static_cast<int>(mouseEvent->button()));
        if (!jsonKey.isEmpty()) {
            applyRecordedJsonKey(jsonKey);
        }
        event->accept();
        return true;
    }
    case QEvent::MouseButtonRelease:
        event->accept();
        return true;
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void KeymapEditorPanel::hideEvent(QHideEvent *event)
{
    setRecordingState(false);
    QWidget::hideEvent(event);
}

void KeymapEditorPanel::closeEvent(QCloseEvent *event)
{
    setRecordingState(false);
    emit closeRequested();
    if (event) {
        event->ignore();
    }
}

void KeymapEditorPanel::applyTheme()
{
    ThemeManager &themeManager = ThemeManager::getInstance();
    const bool darkTheme = themeManager.isDarkTheme();
    setStyleSheet(themeManager.keymapEditorPanelStyleSheet());

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)winId(), darkTheme);
#endif
}

void KeymapEditorPanel::rebuildNodeList()
{
    const QSignalBlocker blocker(m_nodeList);
    m_nodeList->clear();
    if (!m_document) {
        refreshSelection();
        return;
    }

    const QVector<KeymapEditorDocument::NodeInfo> nodes = m_document->nodeInfos();
    int selectedRow = -1;
    for (int i = 0; i < nodes.size(); ++i) {
        const KeymapEditorDocument::NodeInfo &info = nodes.at(i);
        QListWidgetItem *item = new QListWidgetItem(QStringLiteral("[%1] %2%3")
            .arg(info.typeName, info.displayName, info.readOnly ? QStringLiteral(" (read-only)") : QString()), m_nodeList);
        item->setData(Qt::UserRole, info.id);
        if (info.id == m_selectedNodeId) {
            selectedRow = i;
        }
    }
    if (selectedRow >= 0) {
        m_nodeList->setCurrentRow(selectedRow);
    }
    m_addNodeBtn->setEnabled(m_document != nullptr);
    refreshSelection();
}

void KeymapEditorPanel::refreshSelection()
{
    KeymapEditorDocument::NodeInfo info;
    if (m_document && m_selectedNodeId >= 0) {
        info = m_document->nodeInfo(m_selectedNodeId);
    }

    const bool hasSelection = info.id >= 0;
    const QSignalBlocker commentBlocker(m_commentEdit);
    const QSignalBlocker alwaysActiveBlocker(m_alwaysActiveCheck);
    const QSignalBlocker switchMapBlocker(m_switchMapCheck);
    const QSignalBlocker androidKeyBlocker(m_androidKeySpin);

    m_commentEdit->setText(info.comment);
    m_alwaysActiveCheck->setChecked(info.alwaysActive);
    m_switchMapCheck->setChecked(info.switchMap);
    m_primaryKeyEdit->setText(KeymapKeyCodec::displayStringForJsonKey(info.primaryKey));
    m_leftKeyEdit->setText(KeymapKeyCodec::displayStringForJsonKey(info.leftKey));
    m_rightKeyEdit->setText(KeymapKeyCodec::displayStringForJsonKey(info.rightKey));
    m_upKeyEdit->setText(KeymapKeyCodec::displayStringForJsonKey(info.upKey));
    m_downKeyEdit->setText(KeymapKeyCodec::displayStringForJsonKey(info.downKey));
    m_androidKeySpin->setValue(info.androidKey);
    refreshPositionLabels(info);
    refreshPropertyVisibility(info);

    m_deleteNodeBtn->setEnabled(hasSelection && info.canDelete);
    m_primaryRecordBtn->setEnabled(hasSelection && info.supportsPrimaryKey && !info.readOnly);
    m_leftRecordBtn->setEnabled(hasSelection && info.supportsDirectionalKeys && !info.readOnly);
    m_rightRecordBtn->setEnabled(hasSelection && info.supportsDirectionalKeys && !info.readOnly);
    m_upRecordBtn->setEnabled(hasSelection && info.supportsDirectionalKeys && !info.readOnly);
    m_downRecordBtn->setEnabled(hasSelection && info.supportsDirectionalKeys && !info.readOnly);
    updateRecordingButtons();
}

void KeymapEditorPanel::refreshPropertyVisibility(const KeymapEditorDocument::NodeInfo &info)
{
    const bool hasSelection = info.id >= 0;
    m_commentEdit->setEnabled(hasSelection && info.supportsComment && !info.readOnly);
    m_alwaysActiveCheck->setVisible(hasSelection && info.supportsAlwaysActive);
    m_alwaysActiveCheck->setEnabled(hasSelection && info.supportsAlwaysActive && !info.readOnly);
    m_switchMapCheck->setVisible(hasSelection && info.supportsSwitchMap);
    m_switchMapCheck->setEnabled(hasSelection && info.supportsSwitchMap && !info.readOnly);

    m_primaryKeyEdit->parentWidget()->setVisible(hasSelection && info.supportsPrimaryKey);
    m_leftKeyEdit->parentWidget()->setVisible(hasSelection && info.supportsDirectionalKeys);
    m_rightKeyEdit->parentWidget()->setVisible(hasSelection && info.supportsDirectionalKeys);
    m_upKeyEdit->parentWidget()->setVisible(hasSelection && info.supportsDirectionalKeys);
    m_downKeyEdit->parentWidget()->setVisible(hasSelection && info.supportsDirectionalKeys);
    m_androidKeySpin->setVisible(hasSelection && info.supportsAndroidKey);
    m_androidKeySpin->parentWidget()->setVisible(hasSelection && info.supportsAndroidKey);
    m_primaryPosLabel->setVisible(hasSelection && info.hasPrimaryPos);
    m_secondaryPosLabel->setVisible(hasSelection && info.hasSecondaryPos);
    m_smallEyesPosLabel->setVisible(hasSelection && info.hasSmallEyesPos);
}

void KeymapEditorPanel::refreshPositionLabels(const KeymapEditorDocument::NodeInfo &info)
{
    m_primaryPosLabel->setText(info.hasPrimaryPos ? formatPointLabel(info.primaryPos) : QStringLiteral("-"));
    m_secondaryPosLabel->setText(info.hasSecondaryPos ? formatPointLabel(info.secondaryPos) : QStringLiteral("-"));
    m_smallEyesPosLabel->setText(info.hasSmallEyesPos ? formatPointLabel(info.smallEyesPos) : QStringLiteral("-"));
}

void KeymapEditorPanel::setRecordingState(bool enabled, int nodeId, KeymapEditorDocument::KeyBindingField field, const QString &title)
{
    if (!enabled) {
        if (m_recordingActive) {
            qApp->removeEventFilter(this);
        }
        m_recordingActive = false;
        m_recordingNodeId = -1;
        m_recordingField = KeymapEditorDocument::BindingPrimary;
        m_recordingHintLabel->hide();
        m_recordingHintLabel->clear();
        updateRecordingButtons();
        return;
    }

    if (!m_document || nodeId < 0) {
        return;
    }

    setRecordingState(false);
    m_recordingNodeId = nodeId;
    m_recordingField = field;
    m_recordingActive = true;
    m_recordingHintLabel->setText(title);
    m_recordingHintLabel->show();
    updateRecordingButtons();
    QTimer::singleShot(0, this, [this]() {
        if (m_recordingActive) {
            qApp->installEventFilter(this);
        }
    });
}

void KeymapEditorPanel::applyRecordedJsonKey(const QString &jsonKey)
{
    if (!m_document || !m_recordingActive || m_recordingNodeId < 0) {
        return;
    }
    m_document->setKeyBinding(m_recordingNodeId, m_recordingField, jsonKey);
    setRecordingState(false);
}

void KeymapEditorPanel::updateRecordingButtons()
{
    const QString idleText = QStringLiteral("Record");
    const bool primaryListening = m_recordingActive && m_recordingField == KeymapEditorDocument::BindingPrimary;
    const bool leftListening = m_recordingActive && m_recordingField == KeymapEditorDocument::BindingSteerLeft;
    const bool rightListening = m_recordingActive && m_recordingField == KeymapEditorDocument::BindingSteerRight;
    const bool upListening = m_recordingActive && m_recordingField == KeymapEditorDocument::BindingSteerUp;
    const bool downListening = m_recordingActive && m_recordingField == KeymapEditorDocument::BindingSteerDown;

    m_primaryRecordBtn->setText(primaryListening ? QStringLiteral("Listening...") : idleText);
    m_leftRecordBtn->setText(leftListening ? QStringLiteral("Listening...") : idleText);
    m_rightRecordBtn->setText(rightListening ? QStringLiteral("Listening...") : idleText);
    m_upRecordBtn->setText(upListening ? QStringLiteral("Listening...") : idleText);
    m_downRecordBtn->setText(downListening ? QStringLiteral("Listening...") : idleText);

    const QString closeShortcutText = shortcutDisplayText(m_closeKeySequence);
    m_primaryRecordBtn->setToolTip(recordButtonToolTip(tr("主键位"), primaryListening, closeShortcutText));
    m_leftRecordBtn->setToolTip(recordButtonToolTip(tr("左方向键位"), leftListening, closeShortcutText));
    m_rightRecordBtn->setToolTip(recordButtonToolTip(tr("右方向键位"), rightListening, closeShortcutText));
    m_upRecordBtn->setToolTip(recordButtonToolTip(tr("上方向键位"), upListening, closeShortcutText));
    m_downRecordBtn->setToolTip(recordButtonToolTip(tr("下方向键位"), downListening, closeShortcutText));
}

bool KeymapEditorPanel::matchesCloseShortcut(const QKeyEvent *keyEvent) const
{
    return keyEventMatchesShortcut(keyEvent, m_closeKeySequence);
}

QString KeymapEditorPanel::formatPointLabel(const QPointF &point) const
{
    return QStringLiteral("(%1, %2)").arg(point.x(), 0, 'f', 3).arg(point.y(), 0, 'f', 3);
}

bool KeymapEditorPanel::hasMouseMoveNode() const
{
    if (!m_document) {
        return false;
    }
    const QVector<KeymapEditorDocument::NodeInfo> nodes = m_document->nodeInfos();
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).type == KeymapEditorDocument::NodeMouseMove) {
            return true;
        }
    }
    return false;
}
