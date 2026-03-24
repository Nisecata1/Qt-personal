#ifndef KEYMAPEDITORPANEL_H
#define KEYMAPEDITORPANEL_H

#include <QKeySequence>
#include <QPointer>
#include <QWidget>

#include "keymapeditordocument.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSpinBox;
class QLabel;
class QFormLayout;
class QShortcut;
class QCloseEvent;
class QKeyEvent;

class KeymapEditorPanel : public QWidget
{
    Q_OBJECT
public:
    explicit KeymapEditorPanel(QWidget *parent = nullptr);
    ~KeymapEditorPanel() override;

    void setDocument(KeymapEditorDocument *document);
    void setSelectedNodeId(int nodeId);
    int selectedNodeId() const;
    void setScriptDisplayName(const QString &displayName);
    void setCloseShortcut(const QKeySequence &shortcut);

signals:
    void nodeSelected(int nodeId);
    void saveRequested();
    void discardRequested();
    void closeRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void applyTheme();
    void rebuildNodeList();
    void refreshSelection();
    void refreshPropertyVisibility(const KeymapEditorDocument::NodeInfo &info);
    void refreshPositionLabels(const KeymapEditorDocument::NodeInfo &info);
    void setRecordingState(bool enabled, int nodeId = -1, KeymapEditorDocument::KeyBindingField field = KeymapEditorDocument::BindingPrimary, const QString &title = QString());
    void applyRecordedJsonKey(const QString &jsonKey);
    void updateRecordingButtons();
    bool matchesCloseShortcut(const QKeyEvent *keyEvent) const;
    QString formatPointLabel(const QPointF &point) const;
    bool hasMouseMoveNode() const;

    QPointer<KeymapEditorDocument> m_document;
    int m_selectedNodeId = -1;
    int m_recordingNodeId = -1;
    KeymapEditorDocument::KeyBindingField m_recordingField = KeymapEditorDocument::BindingPrimary;
    bool m_recordingActive = false;

    QLabel *m_titleLabel = nullptr;
    QListWidget *m_nodeList = nullptr;
    QComboBox *m_addTypeBox = nullptr;
    QPushButton *m_addNodeBtn = nullptr;
    QPushButton *m_deleteNodeBtn = nullptr;
    QLineEdit *m_commentEdit = nullptr;
    QCheckBox *m_alwaysActiveCheck = nullptr;
    QCheckBox *m_switchMapCheck = nullptr;
    QLineEdit *m_primaryKeyEdit = nullptr;
    QPushButton *m_primaryRecordBtn = nullptr;
    QLineEdit *m_leftKeyEdit = nullptr;
    QPushButton *m_leftRecordBtn = nullptr;
    QLineEdit *m_rightKeyEdit = nullptr;
    QPushButton *m_rightRecordBtn = nullptr;
    QLineEdit *m_upKeyEdit = nullptr;
    QPushButton *m_upRecordBtn = nullptr;
    QLineEdit *m_downKeyEdit = nullptr;
    QPushButton *m_downRecordBtn = nullptr;
    QSpinBox *m_androidKeySpin = nullptr;
    QLabel *m_primaryPosLabel = nullptr;
    QLabel *m_secondaryPosLabel = nullptr;
    QLabel *m_smallEyesPosLabel = nullptr;
    QLabel *m_recordingHintLabel = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_discardBtn = nullptr;
    QShortcut *m_closeShortcut = nullptr;
    QKeySequence m_closeKeySequence = QKeySequence(QStringLiteral("Ctrl+E"));
};

#endif // KEYMAPEDITORPANEL_H
