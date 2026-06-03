#ifndef KEYMAPEDITOROVERLAY_H
#define KEYMAPEDITOROVERLAY_H

#include <QPointer>
#include <QWidget>

#include "keymapeditordocument.h"

class KeymapEditorOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit KeymapEditorOverlay(QWidget *parent = nullptr);

    void setDocument(KeymapEditorDocument *document);
    void setSelectedNodeId(int nodeId);
    int selectedNodeId() const;

signals:
    void nodeSelected(int nodeId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    struct ActiveHandle {
        int nodeId = -1;
        KeymapEditorDocument::HandleRole role = KeymapEditorDocument::HandlePrimaryPos;
        bool valid = false;
    };

    QPointF toPixel(const QPointF &normalizedPos) const;
    QPointF toNormalized(const QPointF &pixelPos) const;
    int hitTestSelectableNode(const QPointF &pixelPos) const;
    ActiveHandle hitTestHandle(const QPointF &pixelPos) const;
    void resetDragState();

    QPointer<KeymapEditorDocument> m_document;
    int m_selectedNodeId = -1;
    ActiveHandle m_activeHandle;
    bool m_dragging = false;
};

#endif // KEYMAPEDITOROVERLAY_H
