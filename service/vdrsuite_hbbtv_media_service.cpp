#include "vdrsuite_hbbtv_media_service.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
constexpr std::size_t MaximumBufferedBytes = 8U * 1024U * 1024U;
constexpr std::size_t MaximumChunkBytes = 1024U * 1024U;

class MediaSocketSource final {
public:
    static std::shared_ptr<MediaSocketSource> Create(
        const std::string& root,
        std::uint64_t revision)
    {
        auto source = std::shared_ptr<MediaSocketSource>(
            new MediaSocketSource());
        if (!source->prepare(root, revision))
            return {};
        try {
            MediaSocketSource* const rawSource = source.get();
            source->writer_ = std::thread([rawSource]() {
                rawSource->writerLoop();
            });
        }
        catch (...) {
            source->closeResources();
            return {};
        }
        return source;
    }

    ~MediaSocketSource()
    {
        stop();
    }

    bool append(const std::uint8_t* data, std::size_t size)
    {
        if (data == nullptr || size == 0 || size > MaximumChunkBytes ||
            stopping_.load(std::memory_order_acquire) ||
            failed_.load(std::memory_order_acquire)) {
            return false;
        }

        std::vector<std::uint8_t> chunk(data, data + size);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queuedBytes_ > MaximumBufferedBytes - size) {
                markFailedLocked();
                return false;
            }
            queuedBytes_ += size;
            queue_.push_back(std::move(chunk));
        }
        wake_.notify_one();
        return true;
    }

    void stop()
    {
        const bool wasStopping =
            stopping_.exchange(true, std::memory_order_acq_rel);

        if (!wasStopping) {
            int listenFd = -1;
            int clientFd = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                listenFd = listenFd_;
                clientFd = clientFd_;
            }

            if (clientFd >= 0)
                ::shutdown(clientFd, SHUT_RDWR);
            if (listenFd >= 0)
                ::shutdown(listenFd, SHUT_RDWR);
        }

        wake_.notify_all();

        if (writer_.joinable())
            writer_.join();

        closeResources();
    }

    bool connected() const
    {
        return connected_.load(std::memory_order_acquire);
    }

    bool failed() const
    {
        return failed_.load(std::memory_order_acquire);
    }

    const std::string& socketPath() const
    {
        return socketPath_;
    }

private:
    MediaSocketSource() = default;

    static bool safeRoot(const std::string& root)
    {
        if (root.empty() || root.front() != '/' || root.size() > 70 ||
            root.find("..") != std::string::npos) {
            return false;
        }
        return std::all_of(
            root.begin(),
            root.end(),
            [](unsigned char character) {
                return std::isalnum(character) != 0 ||
                    character == '/' || character == '-' ||
                    character == '_' || character == '.';
            });
    }

    bool prepare(const std::string& root, std::uint64_t revision)
    {
        if (!safeRoot(root))
            return false;

        if (::mkdir(root.c_str(), 0700) != 0 && errno != EEXIST)
            return false;

        struct stat rootStatus {};
        if (::lstat(root.c_str(), &rootStatus) != 0 ||
            !S_ISDIR(rootStatus.st_mode) ||
            ::chmod(root.c_str(), 0700) != 0) {
            return false;
        }

        socketPath_ =
            root + "/m-" + std::to_string(static_cast<long long>(::getpid())) +
            "-" + std::to_string(revision) + ".sock";
        if (socketPath_.size() >= sizeof(sockaddr_un::sun_path))
            return false;

        ::unlink(socketPath_.c_str());
        listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd_ < 0)
            return false;

        const int flags = ::fcntl(listenFd_, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            closeResources();
            return false;
        }

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(
            address.sun_path,
            socketPath_.c_str(),
            socketPath_.size() + 1);

        if (::bind(
                listenFd_,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != 0 ||
            ::chmod(socketPath_.c_str(), 0600) != 0 ||
            ::listen(listenFd_, 1) != 0) {
            closeResources();
            return false;
        }

        return true;
    }

    void markFailedLocked()
    {
        failed_.store(true, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        if (clientFd_ >= 0)
            ::shutdown(clientFd_, SHUT_RDWR);
        if (listenFd_ >= 0)
            ::shutdown(listenFd_, SHUT_RDWR);
        wake_.notify_all();
    }

    bool sendAll(
        int fd,
        const std::vector<std::uint8_t>& chunk)
    {
        std::size_t offset = 0;
        while (offset < chunk.size()) {
            if (stopping_.load(std::memory_order_acquire))
                return false;

            pollfd descriptor {};
            descriptor.fd = fd;
            descriptor.events = POLLOUT;
            const int polled = ::poll(&descriptor, 1, 250);
            if (polled < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (polled == 0)
                continue;
            if ((descriptor.revents &
                 (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            if ((descriptor.revents & POLLOUT) == 0)
                continue;

            const ssize_t written = ::send(
                fd,
                chunk.data() + offset,
                chunk.size() - offset,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written < 0 &&
                (errno == EINTR ||
                 errno == EAGAIN ||
                 errno == EWOULDBLOCK)) {
                continue;
            }
            return false;
        }
        return true;
    }

    bool acceptClient()
    {
        pollfd descriptor {};
        descriptor.fd = listenFd_;
        descriptor.events = POLLIN;
        const int polled = ::poll(&descriptor, 1, 100);
        if (polled < 0)
            return errno == EINTR;
        if (polled == 0 || (descriptor.revents & POLLIN) == 0)
            return true;
        if ((descriptor.revents &
             (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }

        const int client = ::accept(listenFd_, nullptr, nullptr);
        if (client < 0)
            return errno == EINTR ||
                errno == EAGAIN ||
                errno == EWOULDBLOCK;

        const int flags = ::fcntl(client, F_GETFL, 0);
        if (flags < 0 ||
            ::fcntl(client, F_SETFL, flags | O_NONBLOCK) != 0) {
            ::close(client);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (clientFd_ >= 0 ||
                connected_.load(std::memory_order_acquire)) {
                ::close(client);
                return true;
            }
            clientFd_ = client;
        }
        connected_.store(true, std::memory_order_release);
        return true;
    }

    void writerLoop()
    {
        while (!stopping_.load(std::memory_order_acquire)) {
            int client = -1;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                client = clientFd_;
            }

            if (client < 0) {
                if (!acceptClient()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    markFailedLocked();
                    break;
                }
                continue;
            }

            std::vector<std::uint8_t> chunk;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(
                    lock,
                    std::chrono::milliseconds(100),
                    [this]() {
                        return stopping_.load(std::memory_order_acquire) ||
                            !queue_.empty();
                    });
                if (stopping_.load(std::memory_order_acquire))
                    break;
                if (queue_.empty())
                    continue;

                chunk = std::move(queue_.front());
                queue_.pop_front();
                queuedBytes_ -= chunk.size();
                client = clientFd_;
            }

            if (client < 0 || !sendAll(client, chunk)) {
                std::lock_guard<std::mutex> lock(mutex_);
                markFailedLocked();
                break;
            }
        }
    }

    void closeResources()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (clientFd_ >= 0) {
            ::close(clientFd_);
            clientFd_ = -1;
        }
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (!socketPath_.empty())
            ::unlink(socketPath_.c_str());
        queue_.clear();
        queuedBytes_ = 0;
    }

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::size_t queuedBytes_ = 0;
    int listenFd_ = -1;
    int clientFd_ = -1;
    std::string socketPath_;
    std::thread writer_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> failed_{false};
};

struct MediaContext {
    std::string sessionId;
    std::uint64_t mediaRevision = 0;
    std::uint8_t state = VDRWEB_HBBTV_MEDIA_STATE_NONE;
    bool fullscreen = true;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::shared_ptr<MediaSocketSource> source;
};

std::mutex contextMutex;
std::mutex operationMutex;
MediaContext context;

std::string socketRoot()
{
    const char* configured =
        std::getenv("VDR_SUITE_HBBTV_MEDIA_SOCKET_DIR");
    return configured == nullptr || *configured == '\0'
        ? std::string("/run/vdr/vdr-suite-hbbtv-media")
        : std::string(configured);
}

template <std::size_t Size>
void copyText(char (&target)[Size], const std::string& source)
{
    std::memset(target, 0, Size);
    if (Size == 0)
        return;
    const std::size_t count =
        std::min<std::size_t>(source.size(), Size - 1);
    std::memcpy(target, source.data(), count);
}

std::string boundedText(const char* value, std::size_t maximum)
{
    if (value == nullptr)
        return {};
    const std::size_t size = strnlen(value, maximum);
    if (size == 0 || size >= maximum)
        return {};
    return std::string(value, size);
}

bool validGeometry(int x, int y, int width, int height)
{
    return x >= 0 && y >= 0 &&
        width > 0 && height > 0 &&
        x <= 16384 && y <= 16384 &&
        width <= 16384 && height <= 16384;
}

bool restartVideo()
{
    std::lock_guard<std::mutex> operationLock(operationMutex);

    std::string sessionId;
    std::uint64_t nextRevision = 0;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (context.sessionId.empty())
            return false;
        if (context.mediaRevision ==
            std::numeric_limits<std::uint64_t>::max()) {
            context.state = VDRWEB_HBBTV_MEDIA_STATE_FAILED;
            return false;
        }
        sessionId = context.sessionId;
        nextRevision = context.mediaRevision + 1;
    }

    std::shared_ptr<MediaSocketSource> next =
        MediaSocketSource::Create(socketRoot(), nextRevision);

    std::shared_ptr<MediaSocketSource> previous;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (context.sessionId != sessionId) {
            if (next)
                next->stop();
            return false;
        }

        previous = std::move(context.source);
        context.mediaRevision = nextRevision;
        context.source = next;
        context.state = next
            ? VDRWEB_HBBTV_MEDIA_STATE_STREAMING
            : VDRWEB_HBBTV_MEDIA_STATE_FAILED;
    }

    if (previous)
        previous->stop();
    return static_cast<bool>(next);
}
}

void VdrSuiteHbbtvMediaStore::BeginSession(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> operationLock(operationMutex);
    std::shared_ptr<MediaSocketSource> previous;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        previous = std::move(context.source);
        context = {};
        context.sessionId = sessionId;
        context.fullscreen = true;
    }
    if (previous)
        previous->stop();
}

void VdrSuiteHbbtvMediaStore::EndSession(
    const std::string& sessionId)
{
    std::lock_guard<std::mutex> operationLock(operationMutex);
    std::shared_ptr<MediaSocketSource> previous;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (!sessionId.empty() &&
            context.sessionId != sessionId) {
            return;
        }
        previous = std::move(context.source);
        context = {};
    }
    if (previous)
        previous->stop();
}

bool VdrSuiteHbbtvMediaStore::BeginVideo(
    const std::string&)
{
    return restartVideo();
}

bool VdrSuiteHbbtvMediaStore::ResetVideo(
    const std::string&)
{
    return restartVideo();
}

void VdrSuiteHbbtvMediaStore::StopVideo()
{
    std::lock_guard<std::mutex> operationLock(operationMutex);
    std::shared_ptr<MediaSocketSource> previous;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (context.sessionId.empty())
            return;
        previous = std::move(context.source);
        context.state = VDRWEB_HBBTV_MEDIA_STATE_STOPPED;
    }
    if (previous)
        previous->stop();
}

void VdrSuiteHbbtvMediaStore::PauseVideo()
{
    std::lock_guard<std::mutex> lock(contextMutex);
    if (!context.sessionId.empty() && context.source)
        context.state = VDRWEB_HBBTV_MEDIA_STATE_PAUSED;
}

void VdrSuiteHbbtvMediaStore::ResumeVideo()
{
    std::lock_guard<std::mutex> lock(contextMutex);
    if (!context.sessionId.empty() && context.source)
        context.state = VDRWEB_HBBTV_MEDIA_STATE_STREAMING;
}

void VdrSuiteHbbtvMediaStore::SetVideoSize(
    int x,
    int y,
    int width,
    int height)
{
    if (!validGeometry(x, y, width, height))
        return;

    std::lock_guard<std::mutex> lock(contextMutex);
    if (context.sessionId.empty())
        return;

    context.fullscreen = false;
    context.x = x;
    context.y = y;
    context.width = width;
    context.height = height;
}

void VdrSuiteHbbtvMediaStore::SetVideoFullscreen()
{
    std::lock_guard<std::mutex> lock(contextMutex);
    if (context.sessionId.empty())
        return;

    context.fullscreen = true;
    context.x = 0;
    context.y = 0;
    context.width = 0;
    context.height = 0;
}

bool VdrSuiteHbbtvMediaStore::AppendTs(
    const std::uint8_t* data,
    std::size_t size)
{
    std::shared_ptr<MediaSocketSource> source;
    {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (context.sessionId.empty() || !context.source ||
            (context.state != VDRWEB_HBBTV_MEDIA_STATE_STREAMING &&
             context.state != VDRWEB_HBBTV_MEDIA_STATE_PAUSED)) {
            return false;
        }
        source = context.source;
    }

    const bool accepted = source->append(data, size);
    if (!accepted && source->failed()) {
        std::lock_guard<std::mutex> lock(contextMutex);
        if (context.source == source)
            context.state = VDRWEB_HBBTV_MEDIA_STATE_FAILED;
    }
    return accepted;
}

bool VdrSuiteHbbtvMediaStore::Read(
    VdrWebHbbtvMediaV1& message)
{
    const std::string requestedSession =
        boundedText(
            message.sessionId,
            VDRWEB_HBBTV_SESSION_ID_MAX);

    message.schemaVersion = VDRWEB_HBBTV_MEDIA_SCHEMA_V1;
    message.result = VDRWEB_HBBTV_MEDIA_RESULT_INVALID_REQUEST;
    message.state = VDRWEB_HBBTV_MEDIA_STATE_NONE;
    message.fullscreen = 1;
    message.consumerConnected = 0;
    message.mediaRevision = 0;
    message.x = 0;
    message.y = 0;
    message.width = 0;
    message.height = 0;
    std::memset(
        message.socketPath,
        0,
        sizeof(message.socketPath));

    if (message.structSize != sizeof(VdrWebHbbtvMediaV1) ||
        requestedSession.empty()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(contextMutex);
    if (context.sessionId.empty()) {
        message.result = VDRWEB_HBBTV_MEDIA_RESULT_NO_SESSION;
        return true;
    }
    if (requestedSession != context.sessionId) {
        message.result =
            VDRWEB_HBBTV_MEDIA_RESULT_SESSION_MISMATCH;
        return true;
    }

    message.result = VDRWEB_HBBTV_MEDIA_RESULT_OK;
    message.state = context.state;
    message.fullscreen = context.fullscreen ? 1 : 0;
    message.mediaRevision = context.mediaRevision;
    message.x = context.x;
    message.y = context.y;
    message.width = context.width;
    message.height = context.height;

    if (context.source) {
        if (context.source->failed())
            message.state = VDRWEB_HBBTV_MEDIA_STATE_FAILED;
        message.consumerConnected =
            context.source->connected() ? 1 : 0;
        if (!context.source->failed()) {
            copyText(
                message.socketPath,
                context.source->socketPath());
        }
    }

    return true;
}
