#ifndef AIVISIONPUBLISHER_H
#define AIVISIONPUBLISHER_H

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C"
{
#include "libavutil/frame.h"
#include "libswscale/swscale.h"
}

#pragma pack(push, 1)
struct ScrcpyAIVisionHeaderV1
{
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t seq;
    uint32_t frame_id;
    uint32_t roi_x;
    uint32_t roi_y;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint16_t pixel_format;
    uint64_t timestamp_us;
    uint32_t payload_bytes;
    uint32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(ScrcpyAIVisionHeaderV1) == 48, "ScrcpyAIVisionHeaderV1 size must be 48 bytes");
static_assert(offsetof(ScrcpyAIVisionHeaderV1, seq) == 8, "seq offset must be 8");
static_assert(offsetof(ScrcpyAIVisionHeaderV1, frame_id) == 12, "frame_id offset must be 12");
static_assert(offsetof(ScrcpyAIVisionHeaderV1, payload_bytes) == 40, "payload_bytes offset must be 40");

class AIVisionPublisher
{
public:
    AIVisionPublisher();
    ~AIVisionPublisher();

    void publishBestEffort(const AVFrame *frame);

private:
    bool ensureMappingReady();
    bool prepareBgrRoi(const AVFrame *frame, int &roiX, int &roiY, int &roiW, int &roiH);
    void cleanup();

private:
#if defined(_WIN32)
    void *m_mappingHandle = nullptr;
    unsigned char *m_sharedBase = nullptr;
#endif
    bool m_mappingReady = false;
    SwsContext *m_swsContext = nullptr;
    std::vector<unsigned char> m_bgrBuffer;
    uint32_t m_frameId = 0;
};

#endif // AIVISIONPUBLISHER_H
