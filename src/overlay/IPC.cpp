// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/IPC.h"
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace stellar
{
namespace ipc
{

namespace
{
// Helper to read exactly n bytes from socket
bool
readExact(int socket, uint8_t* buffer, size_t n)
{
    size_t totalRead = 0;
    while (totalRead < n)
    {
        ssize_t r = recv(socket, buffer + totalRead, n - totalRead, 0);
        if (r <= 0)
        {
            return false;
        }
        totalRead += r;
    }
    return true;
}

// Helper to write exactly n bytes to socket
// Uses SO_NOSIGPIPE on macOS or MSG_NOSIGNAL on Linux to avoid SIGPIPE
bool
writeExact(int socket, uint8_t const* buffer, size_t n)
{
    size_t totalWritten = 0;
    while (totalWritten < n)
    {
#ifdef __APPLE__
        // macOS: use SO_NOSIGPIPE socket option (set once per socket)
        // or ignore SIGPIPE (easier for now)
        ssize_t w = send(socket, buffer + totalWritten, n - totalWritten, 0);
#else
        ssize_t w =
            send(socket, buffer + totalWritten, n - totalWritten, MSG_NOSIGNAL);
#endif
        if (w <= 0)
        {
            return false;
        }
        totalWritten += w;
    }
    return true;
}

// RAII helper to ignore SIGPIPE during IPC operations
struct IgnoreSIGPIPE
{
    struct sigaction oldAction;
    IgnoreSIGPIPE()
    {
        struct sigaction action;
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;
        sigaction(SIGPIPE, &action, &oldAction);
    }
    ~IgnoreSIGPIPE()
    {
        sigaction(SIGPIPE, &oldAction, nullptr);
    }
};

} // namespace

bool
sendMessage(int socket, IPCMessage const& msg)
{
    IgnoreSIGPIPE guard;

    // Message format: [type:4 bytes][length:4 bytes][payload]
    uint32_t type = static_cast<uint32_t>(msg.type);
    uint32_t payloadLen = static_cast<uint32_t>(msg.payload.size());

    // Send header
    uint8_t header[8];
    std::memcpy(&header[0], &type, 4);
    std::memcpy(&header[4], &payloadLen, 4);

    if (!writeExact(socket, header, 8))
    {
        return false;
    }

    // Send payload
    if (payloadLen > 0)
    {
        if (!writeExact(socket, msg.payload.data(), payloadLen))
        {
            return false;
        }
    }

    return true;
}

std::optional<IPCMessage>
receiveMessage(int socket)
{
    // Read header
    uint8_t header[8];
    if (!readExact(socket, header, 8))
    {
        return std::nullopt;
    }

    // Parse header
    uint32_t type, payloadLen;
    std::memcpy(&type, &header[0], 4);
    std::memcpy(&payloadLen, &header[4], 4);

    // Sanity check payload length (16MB max)
    if (payloadLen > 16 * 1024 * 1024)
    {
        return std::nullopt;
    }

    // Read payload
    IPCMessage msg;
    msg.type = static_cast<IPCMessageType>(type);
    if (payloadLen > 0)
    {
        msg.payload.resize(payloadLen);
        if (!readExact(socket, msg.payload.data(), payloadLen))
        {
            return std::nullopt;
        }
    }

    return msg;
}

} // namespace ipc

// ============================================================================
// IPCChannel implementation
// ============================================================================

IPCChannel::IPCChannel(int socket) : mSocket(socket), mConnected(true)
{
}

IPCChannel::~IPCChannel()
{
    if (mSocket >= 0)
    {
        close(mSocket);
    }
}

std::unique_ptr<IPCChannel>
IPCChannel::connect(std::string const& socketPath)
{
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return nullptr;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) < 0)
    {
        close(sock);
        return nullptr;
    }

    return std::unique_ptr<IPCChannel>(new IPCChannel(sock));
}

std::unique_ptr<IPCChannel>
IPCChannel::fromSocket(int socket)
{
    return std::unique_ptr<IPCChannel>(new IPCChannel(socket));
}

bool
IPCChannel::send(IPCMessage const& msg)
{
    if (!mConnected)
    {
        return false;
    }

    bool result = ipc::sendMessage(mSocket, msg);
    if (!result)
    {
        mConnected = false;
    }
    return result;
}

std::optional<IPCMessage>
IPCChannel::receive()
{
    if (!mConnected)
    {
        return std::nullopt;
    }

    auto msg = ipc::receiveMessage(mSocket);
    if (!msg)
    {
        mConnected = false;
    }
    return msg;
}

bool
IPCChannel::isConnected() const
{
    return mConnected;
}

// ============================================================================
// IPCServer implementation
// ============================================================================

IPCServer::IPCServer(int socket, std::string socketPath)
    : mSocket(socket), mSocketPath(std::move(socketPath))
{
}

IPCServer::~IPCServer()
{
    if (mSocket >= 0)
    {
        close(mSocket);
    }
    // Clean up socket file
    if (!mSocketPath.empty())
    {
        unlink(mSocketPath.c_str());
    }
}

std::unique_ptr<IPCServer>
IPCServer::create(std::string const& socketPath)
{
    // Remove existing socket file if present
    unlink(socketPath.c_str());

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0)
    {
        return nullptr;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(sock);
        return nullptr;
    }

    if (listen(sock, 1) < 0)
    {
        close(sock);
        unlink(socketPath.c_str());
        return nullptr;
    }

    return std::unique_ptr<IPCServer>(new IPCServer(sock, socketPath));
}

std::unique_ptr<IPCChannel>
IPCServer::accept()
{
    int clientSock = ::accept(mSocket, nullptr, nullptr);
    if (clientSock < 0)
    {
        return nullptr;
    }

    return IPCChannel::fromSocket(clientSock);
}

} // namespace stellar
