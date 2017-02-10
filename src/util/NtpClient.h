#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"

#include <functional>
#include <memory>
#include <string>

namespace stellar
{

/**
 * Very basic async client of NTP protocol. Its only job is to get value
 * of current time from remote NTP server, selected by server paramter
 * in constructor (can be domain name or string representation of IP
 * address).
 *
 * Communication with caller is done with two callback - successCallback
 * gives access to seconds since start of unix epoch and failureCallback
 * informs about problems with receiving information.
 *
 * Please not that communication with NTP is done with UDP protocol and
 * this class does not checks for timeouts. Caller needs to provide
 * checks against timeout.
 */
class NtpClient : public std::enable_shared_from_this<NtpClient>
{
  public:
    explicit NtpClient(asio::io_service& ioService, std::string server,
                       std::function<void(long)> successCallback,
                       std::function<void()> failureCallback);
    ~NtpClient();

    /**
     * Gets time from remote NTP server. This method should be called
     * only once per instance.
     */
    void getTime();

  private:
    const static auto PACKET_SIZE = 48;
    template <typename T> using packet = std::array<T, PACKET_SIZE / sizeof(T)>;

    asio::io_service& mIoService;
    std::string mServer;
    std::function<void(long)> mSuccessCallback;
    std::function<void()> mFailureCallback;

    asio::ip::udp::resolver mResolver;
    std::unique_ptr<asio::ip::udp::socket> mSocket;
    packet<long> mReadBuffer;
    bool mCallbackCalled;

    void success(long time);
    void failure();

    void onServerResolved(asio::error_code ec,
                          asio::ip::udp::resolver::iterator it);
    void onConnectFinished(asio::error_code ec);
    void onWriteFinished(asio::error_code ec);
    void onReadFinished(asio::error_code ec);
};
}
