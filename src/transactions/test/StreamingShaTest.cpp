#include "test/test.h"
#include "test/Catch2.h"
#include "xdr/Stellar-ledger.h"
#include "crypto/SHA.h"
#include "crypto/Hex.h"
#include "crypto/ByteSlice.h"
#include <vector>
#include <xdrpp/marshal.h>
#include <iostream>
#include <arpa/inet.h>

using namespace stellar;

TEST_CASE("Streaming SHA256 for InvokeHostFunctionSuccessPreImage", "[tx][streaming_sha]") {
    InvokeHostFunctionSuccessPreImage preImage;
    
    // 1. Setup returnValue (SCVal)
    // Let's make it a simple U32
    preImage.returnValue.type(SCV_U32);
    preImage.returnValue.u32() = 0xDEADBEEF;

    // 2. Setup events
    // Add a couple of events
    ContractEvent event1;
    event1.type = DIAGNOSTIC;
    event1.body.v0().topics.resize(1);
    event1.body.v0().topics[0].type(SCV_SYMBOL);
    event1.body.v0().topics[0].sym() = "Topic1";
    event1.body.v0().data.type(SCV_U32);
    event1.body.v0().data.u32() = 123;
    preImage.events.push_back(event1);

    ContractEvent event2;
    event2.type = SYSTEM;
    event2.body.v0().topics.resize(2);
    event2.body.v0().topics[0].type(SCV_SYMBOL);
    event2.body.v0().topics[0].sym() = "Topic2";
    event2.body.v0().topics[1].type(SCV_I32);
    event2.body.v0().topics[1].i32() = -42;
    event2.body.v0().data.type(SCV_VOID);
    preImage.events.push_back(event2);

    // --- Benchmark & Verify xdrSha256 ---
    auto start = std::chrono::high_resolution_clock::now();
    Hash hash1 = xdrSha256(preImage);
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "xdrSha256 time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() << "ns" << std::endl;

    // --- Prepare Streaming ---
    // In the real implementation, we would have raw bytes from the host.
    // Here we simulate that by pre-serializing the components.
    
    xdr::xvector<uint8_t> returnValueBytes = xdr::xdr_to_opaque(preImage.returnValue);
    std::vector<xdr::xvector<uint8_t>> eventsBytes;
    for (const auto& event : preImage.events) {
        eventsBytes.push_back(xdr::xdr_to_opaque(event));
    }

    // --- Run Streaming SHA256 ---
    start = std::chrono::high_resolution_clock::now();
    SHA256 sha;
    
    // 1. returnValue bytes
    sha.add(returnValueBytes);
    
    // 2. events length (4 bytes big endian)
    uint32_t eventsSize = static_cast<uint32_t>(preImage.events.size());
    uint32_t eventsSizeBe = htonl(eventsSize); // Use htonl for network byte order (Big Endian)
    sha.add(ByteSlice(reinterpret_cast<const uint8_t*>(&eventsSizeBe), 4));

    // 3. events bytes
    for (const auto& eventBytes : eventsBytes) {
        sha.add(eventBytes);
    }
    
    Hash hash2 = sha.finish();
    end = std::chrono::high_resolution_clock::now();
    std::cout << "Streaming time: " << std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() << "ns" << std::endl;

    // --- Verify ---
    if (hash1 != hash2) {
        std::cout << "MISMATCH!" << std::endl;
        std::cout << "Hash1 (xdrSha256): " << binToHex(hash1) << std::endl;
        std::cout << "Hash2 (Streaming): " << binToHex(hash2) << std::endl;
    }
    REQUIRE(hash1 == hash2);
}
