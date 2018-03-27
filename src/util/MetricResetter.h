#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "medida/metrics_registry.h"

namespace medida
{
class MetricsRegistry;
class Meter;
class Counter;
class Timer;
}

namespace stellar
{

class MetricResetter : public medida::MetricProcessor
{
  public:
    MetricResetter() = default;
    ~MetricResetter() override = default;
    void Process(medida::Counter& counter) override;
    void Process(medida::Meter& meter) override;
    void Process(medida::Histogram& histogram) override;
    void Process(medida::Timer& timer) override;
};
}
