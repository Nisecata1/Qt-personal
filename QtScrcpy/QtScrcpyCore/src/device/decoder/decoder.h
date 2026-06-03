#ifndef DECODER_H
#define DECODER_H
#include <QObject>
#include <memory>

extern "C"
{
#include "libavcodec/avcodec.h"
}

#include <functional>

class VideoBuffer;
class AIVisionPublisher;
class Decoder : public QObject
{
    Q_OBJECT
public:
    Decoder(std::function<void(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV, int linesizeY, int linesizeU, int linesizeV)> onFrame, QObject *parent = Q_NULLPTR);
    virtual ~Decoder();

    bool open();
    void close();
    bool push(const AVPacket *packet);
    void peekFrame(std::function<void(int width, int height, uint8_t* dataRGB32)> onFrame);

signals:
    void updateFPS(quint32 fps);

private slots:
    void onNewFrame();

signals:
    void newFrame();

private:
    void pushFrame();

private:
    VideoBuffer *m_vb = Q_NULLPTR;
    AVCodecContext *m_codecCtx = Q_NULLPTR;
    bool m_isCodecCtxOpen = false;
    std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> m_onFrame = Q_NULLPTR;
    std::unique_ptr<AIVisionPublisher> m_aiVisionPublisher;
};

#endif // DECODER_H
