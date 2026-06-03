#include "keymapeditoroverlay.h"

#include <cmath>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>

namespace {
constexpr int kHandleRadius = 10;
constexpr int kHitDistance = 14;
constexpr qreal kLabelOffsetX = 12.0;
constexpr qreal kLabelOffsetY = 12.0;
constexpr qreal kLabelHeight = 24.0;
constexpr qreal kLabelPadding = 6.0;
constexpr qreal kMaxLabelWidth = 140.0;

QFont overlayFont()
{
    return QFont(QStringLiteral("Microsoft YaHei UI"), 9);
}

QRectF labelRectForHandle(const QPointF &center, const QString &label)
{
    const QFontMetricsF metrics(overlayFont());
    const qreal labelWidth = qMin(kMaxLabelWidth, metrics.horizontalAdvance(label) + kLabelPadding);
    return QRectF(center.x() + kLabelOffsetX, center.y() - kLabelOffsetY, labelWidth, kLabelHeight);
}
}

KeymapEditorOverlay::KeymapEditorOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAttribute(Qt::WA_StyledBackground, false);
    setMouseTracking(true);
}

void KeymapEditorOverlay::setDocument(KeymapEditorDocument *document)
{
    if (m_document == document) {
        return;
    }

    if (m_document) {
        disconnect(m_document, nullptr, this, nullptr);
    }

    m_document = document;
    if (m_document) {
        connect(m_document, &KeymapEditorDocument::documentReset, this, [this]() {
            m_selectedNodeId = -1;
            update();
        });
        connect(m_document, &KeymapEditorDocument::nodeListChanged, this, [this]() {
            update();
        });
        connect(m_document, &KeymapEditorDocument::nodeChanged, this, [this](int) {
            update();
        });
    }
    update();
}

void KeymapEditorOverlay::setSelectedNodeId(int nodeId)
{
    if (m_selectedNodeId == nodeId) {
        return;
    }
    m_selectedNodeId = nodeId;
    update();
}

int KeymapEditorOverlay::selectedNodeId() const
{
    return m_selectedNodeId;
}

void KeymapEditorOverlay::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(0, 0, 0, 48));

    if (!m_document) {
        return;
    }

    const QVector<KeymapEditorDocument::NodeInfo> nodes = m_document->nodeInfos();
    for (int i = 0; i < nodes.size(); ++i) {
        const KeymapEditorDocument::NodeInfo &info = nodes.at(i);
        if (info.type == KeymapEditorDocument::NodeDrag && info.hasPrimaryPos && info.hasSecondaryPos) {
            painter.setPen(QPen(QColor(120, 200, 255, 190), 2.0));
            painter.drawLine(toPixel(info.primaryPos), toPixel(info.secondaryPos));
        } else if (info.type == KeymapEditorDocument::NodeMouseMove && info.hasPrimaryPos && info.hasSmallEyesPos) {
            painter.setPen(QPen(QColor(255, 180, 80, 180), 1.5, Qt::DashLine));
            painter.drawLine(toPixel(info.primaryPos), toPixel(info.smallEyesPos));
        }
    }

    const QVector<KeymapEditorDocument::HandleInfo> handles = m_document->handleInfos(m_selectedNodeId);
    painter.setFont(overlayFont());
    for (int i = 0; i < handles.size(); ++i) {
        const KeymapEditorDocument::HandleInfo &handle = handles.at(i);
        const QPointF center = toPixel(handle.normalizedPos);
        const QColor base = handle.readOnly ? QColor(120, 120, 120, 220)
                                            : (handle.selected ? QColor(80, 170, 255, 240) : QColor(255, 255, 255, 220));
        painter.setPen(QPen(QColor(20, 20, 20, 220), 2.0));
        painter.setBrush(base);
        painter.drawEllipse(center, kHandleRadius, kHandleRadius);
        painter.setPen(QColor(240, 240, 240));
        painter.drawText(labelRectForHandle(center, handle.label), handle.label);
    }
}

void KeymapEditorOverlay::mousePressEvent(QMouseEvent *event)
{
    if (!m_document || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const int selectedNodeId = hitTestSelectableNode(event->pos());
    if (selectedNodeId >= 0) {
        if (m_selectedNodeId != selectedNodeId) {
            m_selectedNodeId = selectedNodeId;
            emit nodeSelected(m_selectedNodeId);
        }
        const ActiveHandle handle = hitTestHandle(event->pos());
        if (handle.valid) {
            m_activeHandle = handle;
            m_dragging = true;
        } else {
            resetDragState();
        }
        update();
        event->accept();
        return;
    }

    event->accept();
}

void KeymapEditorOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_document || !m_dragging || !m_activeHandle.valid) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    m_document->setHandlePosition(m_activeHandle.nodeId, m_activeHandle.role, toNormalized(event->pos()));
    event->accept();
}

void KeymapEditorOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        resetDragState();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

QPointF KeymapEditorOverlay::toPixel(const QPointF &normalizedPos) const
{
    return QPointF(normalizedPos.x() * width(), normalizedPos.y() * height());
}

QPointF KeymapEditorOverlay::toNormalized(const QPointF &pixelPos) const
{
    if (width() <= 0 || height() <= 0) {
        return QPointF(0.0, 0.0);
    }
    const qreal x = qBound(0.0, pixelPos.x() / width(), 1.0);
    const qreal y = qBound(0.0, pixelPos.y() / height(), 1.0);
    return QPointF(x, y);
}

int KeymapEditorOverlay::hitTestSelectableNode(const QPointF &pixelPos) const
{
    if (!m_document) {
        return -1;
    }

    const QVector<KeymapEditorDocument::HandleInfo> handles = m_document->handleInfos(m_selectedNodeId);
    for (int i = 0; i < handles.size(); ++i) {
        const KeymapEditorDocument::HandleInfo &handle = handles.at(i);
        const QPointF center = toPixel(handle.normalizedPos);
        const QPointF delta = center - pixelPos;
        if (std::hypot(delta.x(), delta.y()) <= kHitDistance) {
            return handle.nodeId;
        }
        if (labelRectForHandle(center, handle.label).contains(pixelPos)) {
            return handle.nodeId;
        }
    }

    return -1;
}

KeymapEditorOverlay::ActiveHandle KeymapEditorOverlay::hitTestHandle(const QPointF &pixelPos) const
{
    ActiveHandle result;
    if (!m_document) {
        return result;
    }

    const QVector<KeymapEditorDocument::HandleInfo> handles = m_document->handleInfos(m_selectedNodeId);
    for (int i = 0; i < handles.size(); ++i) {
        const KeymapEditorDocument::HandleInfo &handle = handles.at(i);
        const QPointF delta = toPixel(handle.normalizedPos) - pixelPos;
        if (handle.movable && std::hypot(delta.x(), delta.y()) <= kHitDistance) {
            result.nodeId = handle.nodeId;
            result.role = handle.role;
            result.valid = true;
            return result;
        }
    }
    return result;
}

void KeymapEditorOverlay::resetDragState()
{
    m_dragging = false;
    m_activeHandle = ActiveHandle();
}
