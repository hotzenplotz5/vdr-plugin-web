#include <iostream>
#include <mutex>

#include "debuglog.h"
#include "VdrClient.h"
#include <thrift/TOutput.h>
#include "TransportBuffer.h"

using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace ::pluginweb;
using namespace ::common;


void vdrClientOutputFunction(const char* msg) {
    DEBUGLOG("%s\n", msg);
}

VdrClient::VdrClient(std::string vdrIp, int vdrPort) {
    DEBUGLOG("Construct VdrClient");

    socket = std::make_shared<TSocket>(vdrIp, vdrPort);
    socket->setConnTimeout(3000);
    socket->setSendTimeout(10000);
    socket->setRecvTimeout(10000);
    transport = std::make_shared<TBufferedTransport>(
        socket, THRIFT_TRANSPORT_BUFFER_SIZE, THRIFT_TRANSPORT_BUFFER_SIZE);
    std::shared_ptr<TProtocol> protocol(new TBinaryProtocol(transport));

    client = new VdrPluginWebClient(protocol);

    GlobalOutput.setOutputFunction(vdrClientOutputFunction);
}

VdrClient::~VdrClient() {
    DEBUGLOG("Destruct VdrClient");

    // Do not flush a potentially broken buffered transport here.
    socket->close();
    delete client;
}

bool VdrClient::connect() {
    if (!transport->isOpen()) {
        try {
            transport->open();
        } catch (TTransportException&) {
            return false;
        }
    }

    return true;
}

void VdrClient::reset() {
    socket->close();
    transport = std::make_shared<TBufferedTransport>(
        socket, THRIFT_TRANSPORT_BUFFER_SIZE, THRIFT_TRANSPORT_BUFFER_SIZE);
    delete client;
    client = new VdrPluginWebClient(std::make_shared<TBinaryProtocol>(transport));
}

bool VdrClient::ping() {
    std::lock_guard<std::recursive_mutex> lock(vdr_send_mutex);

    if (!connect())
        return false;

    try {
        client->ping();
        return true;
    } catch (OperationFailed&) {
        return false;
    } catch (TException&) {
        reset();
        return false;
    }
}

template <typename F> bool VdrClient::processInternal(F&& request) {
    std::lock_guard<std::recursive_mutex> lock(vdr_send_mutex);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!connect())
            return false;

        try {
            return request();
        } catch (OperationFailed&) {
            return false;
        } catch (TTransportException &err) {
            reset();
            if (err.getType() == TTransportException::TIMED_OUT)
                return false;
        } catch (TException&) {
            reset();
            return false;
        }
    }

    return false;
}
bool VdrClient::ProcessOsdUpdate(int disp_width, int disp_height, int x, int y, int width, int height, std::string& data) {
    // DEBUGLOG("VdrClient::ProcessOsdUpdate");

    return processInternal([&]() -> bool {
        ProcessOsdUpdateType input;
        input.disp_width = disp_width;
        input.disp_height = disp_height;
        input.x = x;
        input.y = y;
        input.width = width;
        input.height = height;
        input.data = data;

        return client->ProcessOsdUpdate(input);
    });
}

bool VdrClient::ProcessOsdUpdateQoi(int disp_width, int disp_height, int x, int y, const std::string &imageQoi) {
    // DEBUGLOG("VdrClient::ProcessOsdUpdateQoi");

    return processInternal([&]() -> bool {
        ProcessOsdUpdateQOIType input;
        input.x = x;
        input.y = y;
        input.render_width = disp_width;
        input.render_height = disp_height;
        input.image_data = imageQoi;

        return client->ProcessOsdUpdateQOI(input);
    });
}

bool VdrClient::ProcessOsdFrame(
        int render_width,
        int render_height,
        bool qoi,
        const std::vector<OsdRectType>& rects) {
    return processInternal([&]() -> bool {
        ProcessOsdFrameType input;
        input.render_width = render_width;
        input.render_height = render_height;
        input.qoi = qoi;
        input.rects = rects;
        return client->ProcessOsdFrame(input);
    });
}

bool VdrClient::ProcessTSPacket(std::string& packets) {
    // DEBUGLOG("VdrClient::ProcessTSPacket");

    return processInternal([&]() -> bool {
        ProcessTSPacketType input;
        input.ts = packets;

        return client->ProcessTSPacket(input);
    });
}

bool VdrClient::StartVideo(std::string& videoInfo, bool mse) {
    DEBUGLOG("VdrClient::StartVideo");

    return processInternal([&]() -> bool {
        StartVideoType input;
        input.videoInfo = videoInfo;
        input.mse = mse;

        return client->StartVideo(input);
    });
}

bool VdrClient::StopVideo() {
    DEBUGLOG("VdrClient::StopVideo");

    return processInternal([&]() -> bool {
        return client->StopVideo();
    });
}

bool VdrClient::Pause() {
    DEBUGLOG("VdrClient::Pause");

    return processInternal([&]() -> bool {
        return client->PauseVideo();
    });
}

bool VdrClient::Resume() {
    DEBUGLOG("VdrClient::Resume");

    return processInternal([&]() -> bool {
        return client->ResumeVideo();
    });
}

bool VdrClient::ResetVideo(std::string& videoInfo, bool mse) {
    DEBUGLOG("VdrClient::ResetVideo");

    return processInternal([&]() -> bool {
        ResetVideoType input;
        input.videoInfo = videoInfo;
        input.mse = mse;

        return client->ResetVideo(input);
    });
}

bool VdrClient::Seeked() {
    DEBUGLOG("VdrClient::Seeked");

    return processInternal([&]() -> bool {
        return client->Seeked();
    });
}

bool VdrClient::VideoSize(int x, int y, int w, int h) {
    DEBUGLOG("VdrClient::VideoSize");

    return processInternal([&]() -> bool {
        VideoSizeType input;
        input.x = x;
        input.y = y;
        input.w = w;
        input.h = h;

        return client->VideoSize(input);
    });
}

bool VdrClient::VideoFullscreen() {
    DEBUGLOG("VdrClient::VideoFullscreen");

    return processInternal([&]() -> bool {
        return client->VideoFullscreen();
    });
}

bool VdrClient::SelectAudioTrack(std::string& nr) {
    DEBUGLOG("VdrClient::SelectAudioTrack");

    return processInternal([&]() -> bool {
        SelectAudioTrackType input;
        input.audioTrack = nr;

        return client->SelectAudioTrack(input);
    });
}

bool VdrClient::SelectSubtitleTrack(const std::string& nr) {
    DEBUGLOG("VdrClient::SelectSubtitleTrack");

    return processInternal([&]() -> bool {
        SelectSubtitleTrackType input;
        input.subtitleTrack = nr;
        return client->SelectSubtitleTrack(input);
    });
}

bool VdrClient::EndOfStream() {
    DEBUGLOG("VdrClient::EndOfStream");

    return processInternal([&]() -> bool {
        return client->EndOfStream();
    });
}

bool VdrClient::StreamEventFilter(int componentTag, bool enable) {
    DEBUGLOG("VdrClient::StreamEventFilter");

    return processInternal([&]() -> bool {
        StreamEventFilterType input;
        input.componentTag = componentTag;
        input.enable = enable;
        return client->StreamEventFilter(input);
    });
}

bool VdrClient::IsWebActive() {
    // DEBUGLOG("VdrClient::IsWebActive");

    return processInternal([&]() -> bool {
        return client->IsWebActive();
    });
}
