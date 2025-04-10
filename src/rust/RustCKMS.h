#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "rust/RustBridge.h"
#include "medida/stats/ckms.h"
#include <Tracy.hpp>

namespace stellar {
    class RustCKMS : public medida::stats::CKMS {
        rust::Box<rust_bridge::CkmsState> impl;
    public:
        RustCKMS() : impl(rust_bridge::ckms_new()) {}
        ~RustCKMS() override = default;

        void insert(double value) override {
            ZoneScoped;
            impl->ckms_insert(value);
        }
        double get(double q) override { return impl->ckms_get(q); }
        void reset() override { impl->ckms_reset(); }
        std::size_t count() const override { return impl->ckms_count(); }
        double max() const override { return impl->ckms_max(); }

        // Factory function for medida registration
        static std::shared_ptr<medida::stats::CKMS> create() {
            return std::shared_ptr<medida::stats::CKMS>(new RustCKMS());
        }
    };

    // Function to register RustCKMS as the default CKMS implementation
    inline void register_rust_ckms() {
        medida::stats::createCKMS = RustCKMS::create;
    }
}
