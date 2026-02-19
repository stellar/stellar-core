// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "overlay/IPC.h"
#include "util/TmpDir.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace stellar;

TEST_CASE("IPC socket creation", "[overlay][ipc]")
{
    SECTION("create unix domain socket pair")
    {
        int sockets[2];
        // Use SOCK_STREAM since SOCK_SEQPACKET not available on macOS
        int result = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
        REQUIRE(result == 0);

        close(sockets[0]);
        close(sockets[1]);
    }
}

TEST_CASE("IPC message framing", "[overlay][ipc]")
{
    SECTION("send and receive small message")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        // Message format: [type:4 bytes][length:4 bytes][payload]
        uint32_t msgType = 42;
        std::string payload = "hello";
        uint32_t payloadLen = payload.size();

        // Sender
        {
            std::vector<uint8_t> msg;
            msg.resize(8 + payloadLen);
            std::memcpy(&msg[0], &msgType, 4);
            std::memcpy(&msg[4], &payloadLen, 4);
            std::memcpy(&msg[8], payload.data(), payloadLen);

            ssize_t sent = send(sockets[0], msg.data(), msg.size(), 0);
            REQUIRE(sent == msg.size());
        }

        // Receiver
        {
            std::vector<uint8_t> buf(1024);
            ssize_t received = recv(sockets[1], buf.data(), buf.size(), 0);
            REQUIRE(received == 8 + payloadLen);

            uint32_t recvType, recvLen;
            std::memcpy(&recvType, &buf[0], 4);
            std::memcpy(&recvLen, &buf[4], 4);

            REQUIRE(recvType == msgType);
            REQUIRE(recvLen == payloadLen);

            std::string recvPayload(buf.begin() + 8, buf.begin() + 8 + recvLen);
            REQUIRE(recvPayload == payload);
        }

        close(sockets[0]);
        close(sockets[1]);
    }
}

TEST_CASE("IPC message types", "[overlay][ipc]")
{
    SECTION("CoreToOverlay::BroadcastSCP")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        // Create a BroadcastSCP message
        IPCMessage msg;
        msg.type = IPCMessageType::BROADCAST_SCP;
        msg.payload = std::vector<uint8_t>{1, 2, 3, 4, 5}; // mock XDR envelope

        // Send
        REQUIRE(ipc::sendMessage(sockets[0], msg));

        // Receive
        auto recvMsg = ipc::receiveMessage(sockets[1]);
        REQUIRE(recvMsg.has_value());
        REQUIRE(recvMsg->type == IPCMessageType::BROADCAST_SCP);
        REQUIRE(recvMsg->payload == msg.payload);

        close(sockets[0]);
        close(sockets[1]);
    }

    SECTION("OverlayToCore::SCPReceived")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        // Create a SCPReceived message
        IPCMessage msg;
        msg.type = IPCMessageType::SCP_RECEIVED;
        msg.payload = std::vector<uint8_t>{5, 4, 3, 2, 1}; // mock XDR envelope

        // Send
        REQUIRE(ipc::sendMessage(sockets[0], msg));

        // Receive
        auto recvMsg = ipc::receiveMessage(sockets[1]);
        REQUIRE(recvMsg.has_value());
        REQUIRE(recvMsg->type == IPCMessageType::SCP_RECEIVED);
        REQUIRE(recvMsg->payload == msg.payload);

        close(sockets[0]);
        close(sockets[1]);
    }
}

TEST_CASE("IPC error handling", "[overlay][ipc]")
{
    SECTION("receive on closed socket returns nullopt")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        close(sockets[0]); // Close sender

        auto msg = ipc::receiveMessage(sockets[1]);
        REQUIRE(!msg.has_value());

        close(sockets[1]);
    }

    SECTION("send on closed socket returns false")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        close(sockets[1]); // Close receiver

        IPCMessage msg;
        msg.type = IPCMessageType::BROADCAST_SCP;
        msg.payload = std::vector<uint8_t>{1, 2, 3};

        REQUIRE(!ipc::sendMessage(sockets[0], msg));

        close(sockets[0]);
    }
}

TEST_CASE("IPC large message", "[overlay][ipc]")
{
    SECTION("send and receive 10MB message")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        // Create 10MB TX set
        IPCMessage msg;
        msg.type = IPCMessageType::TX_SET_AVAILABLE;
        msg.payload.resize(10 * 1024 * 1024);
        for (size_t i = 0; i < msg.payload.size(); ++i)
        {
            msg.payload[i] = static_cast<uint8_t>(i % 256);
        }

        // Need to send/receive concurrently for large messages
        // because socket buffers are smaller than 10MB
        std::optional<IPCMessage> recvMsg;
        std::thread receiver(
            [&]() { recvMsg = ipc::receiveMessage(sockets[1]); });

        // Send
        bool sent = ipc::sendMessage(sockets[0], msg);

        receiver.join();

        REQUIRE(sent);
        REQUIRE(recvMsg.has_value());
        REQUIRE(recvMsg->type == IPCMessageType::TX_SET_AVAILABLE);
        REQUIRE(recvMsg->payload.size() == msg.payload.size());
        REQUIRE(recvMsg->payload == msg.payload);

        close(sockets[0]);
        close(sockets[1]);
    }
}

TEST_CASE("IPCChannel", "[overlay][ipc]")
{
    SECTION("create from socket pair")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        auto channel1 = IPCChannel::fromSocket(sockets[0]);
        auto channel2 = IPCChannel::fromSocket(sockets[1]);

        REQUIRE(channel1 != nullptr);
        REQUIRE(channel2 != nullptr);
        REQUIRE(channel1->isConnected());
        REQUIRE(channel2->isConnected());

        // Send from channel1, receive on channel2
        IPCMessage msg;
        msg.type = IPCMessageType::BROADCAST_SCP;
        msg.payload = {1, 2, 3, 4, 5};

        REQUIRE(channel1->send(msg));

        auto recvMsg = channel2->receive();
        REQUIRE(recvMsg.has_value());
        REQUIRE(recvMsg->type == IPCMessageType::BROADCAST_SCP);
        REQUIRE(recvMsg->payload == msg.payload);
    }

    SECTION("connection closed detection")
    {
        int sockets[2];
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

        auto channel1 = IPCChannel::fromSocket(sockets[0]);
        auto channel2 = IPCChannel::fromSocket(sockets[1]);

        // Close channel1
        channel1.reset();

        // channel2 should detect connection closed on receive
        auto msg = channel2->receive();
        REQUIRE(!msg.has_value());
        REQUIRE(!channel2->isConnected());
    }
}

TEST_CASE("IPCServer and connect", "[overlay][ipc]")
{
    SECTION("server accept and client connect")
    {
        TmpDir tmpDir("ipc-test");
        std::string socketPath = tmpDir.getName() + "/test.sock";

        // Create server in another thread
        std::unique_ptr<IPCChannel> serverChannel;
        std::thread serverThread([&]() {
            auto server = IPCServer::create(socketPath);
            REQUIRE(server != nullptr);
            serverChannel = server->accept();
        });

        // Give server time to start listening
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Connect client
        auto clientChannel = IPCChannel::connect(socketPath);

        serverThread.join();

        REQUIRE(clientChannel != nullptr);
        REQUIRE(serverChannel != nullptr);
        REQUIRE(clientChannel->isConnected());
        REQUIRE(serverChannel->isConnected());

        // Send message from client to server
        IPCMessage msg;
        msg.type = IPCMessageType::SCP_RECEIVED;
        msg.payload = {10, 20, 30};

        REQUIRE(clientChannel->send(msg));

        auto recvMsg = serverChannel->receive();
        REQUIRE(recvMsg.has_value());
        REQUIRE(recvMsg->type == IPCMessageType::SCP_RECEIVED);
        REQUIRE(recvMsg->payload == msg.payload);

        // Send message from server to client
        IPCMessage response;
        response.type = IPCMessageType::BROADCAST_SCP;
        response.payload = {100, 200};

        REQUIRE(serverChannel->send(response));

        auto recvResponse = clientChannel->receive();
        REQUIRE(recvResponse.has_value());
        REQUIRE(recvResponse->type == IPCMessageType::BROADCAST_SCP);
        REQUIRE(recvResponse->payload == response.payload);
    }
}

TEST_CASE("IPC cross-process communication", "[overlay][ipc-crossproc][.]")
{
    // This test uses the overlay-stub binary
    // Skip by default since it requires the binary to be built

    TmpDir tmpDir("ipc-cross-process");
    std::string socketPath = tmpDir.getName() + "/overlay.sock";

    // Find the overlay-stub binary (built separately)
    // Try multiple paths since CWD can vary
    std::string overlayStubPath;
    std::vector<std::string> paths = {
        "overlay-stub/target/release/overlay-stub",
        "../overlay-stub/target/release/overlay-stub",
    };
    for (auto const& p : paths)
    {
        if (access(p.c_str(), X_OK) == 0)
        {
            overlayStubPath = p;
            break;
        }
    }
    REQUIRE_FALSE(overlayStubPath.empty());

    // Start overlay-stub process
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process - exec overlay-stub
        execl(overlayStubPath.c_str(), "overlay-stub", socketPath.c_str(),
              nullptr);
        _exit(1); // exec failed
    }

    REQUIRE(pid > 0);

    // Give overlay-stub time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Connect to overlay-stub
    auto channel = IPCChannel::connect(socketPath);
    REQUIRE(channel != nullptr);
    REQUIRE(channel->isConnected());

    SECTION("BroadcastSCP echoed as SCPReceived")
    {
        IPCMessage msg;
        msg.type = IPCMessageType::BROADCAST_SCP;
        msg.payload = {1, 2, 3, 4, 5};

        REQUIRE(channel->send(msg));

        auto response = channel->receive();
        REQUIRE(response.has_value());
        REQUIRE(response->type == IPCMessageType::SCP_RECEIVED);
        REQUIRE(response->payload == msg.payload);
    }

    SECTION("GetTopTxs returns mock transactions")
    {
        IPCMessage msg;
        msg.type = IPCMessageType::GET_TOP_TXS;
        // Payload: [count:4]
        uint32_t count = 100;
        msg.payload.resize(4);
        memcpy(msg.payload.data(), &count, 4);

        REQUIRE(channel->send(msg));

        auto response = channel->receive();
        REQUIRE(response.has_value());
        REQUIRE(response->type == IPCMessageType::TOP_TXS_RESPONSE);
        // Response: [count:4][len1:4][tx1:len1]... - at least 4 bytes for count
        REQUIRE(response->payload.size() >= 4);
    }

    // Send shutdown
    {
        IPCMessage shutdown;
        shutdown.type = IPCMessageType::SHUTDOWN;
        channel->send(shutdown);
    }

    // Wait for child
    int status;
    waitpid(pid, &status, 0);
}
