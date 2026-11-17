#pragma once

#include <memory>
#include <thrift/transport/TBufferTransports.h>

#define THRIFT_TRANSPORT_BUFFER_SIZE (256 * 1024)

class TLargeBufferedTransportFactory : public apache::thrift::transport::TTransportFactory {
public:
    std::shared_ptr<apache::thrift::transport::TTransport> getTransport(
        std::shared_ptr<apache::thrift::transport::TTransport> trans) override
    {
        return std::make_shared<apache::thrift::transport::TBufferedTransport>(
            trans, THRIFT_TRANSPORT_BUFFER_SIZE, THRIFT_TRANSPORT_BUFFER_SIZE);
    }
};
