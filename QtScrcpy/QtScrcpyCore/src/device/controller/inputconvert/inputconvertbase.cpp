#include "inputconvertbase.h"
#include "controller.h"

InputConvertBase::InputConvertBase(Controller *controller) : QObject(controller), m_controller(controller)
{
    Q_ASSERT(controller);
}

InputConvertBase::~InputConvertBase() {}

void InputConvertBase::resetInputState()
{
    // 默认输入转换器没有额外状态；子类只在自己持有触点、定时器或光标状态时覆盖这里。
}

void InputConvertBase::sendControlMsg(ControlMsg *msg)
{
    // 为普通输入转换提供默认优先级的异步控制消息发送入口。
    sendControlMsg(msg, Qt::NormalEventPriority);
}

void InputConvertBase::sendControlMsg(ControlMsg *msg, int priority)
{
    // 将普通输入转换生成的控制消息交给 Controller 排队发送。
    if (msg && m_controller) {
        m_controller->postControlMsg(msg, priority);
    }
}

bool InputConvertBase::sendControlMsgImmediately(ControlMsg *msg)
{
    // 为高频输入转换提供立即发送入口；没有 Controller 时主动释放消息避免泄漏。
    if (!msg) {
        return false;
    }
    if (!m_controller) {
        delete msg;
        return false;
    }
    return m_controller->sendControlMsgImmediately(msg);
}

qint64 InputConvertBase::pendingControlBytes() const
{
    // 给输入转换器读取控制通道待写字节数，用于判断高频位置消息是否已经积压。
    if (!m_controller) {
        return 0;
    }
    return m_controller->pendingControlBytes();
}
