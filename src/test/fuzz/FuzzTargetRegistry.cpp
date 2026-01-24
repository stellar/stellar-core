// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/fuzz/FuzzTargetRegistry.h"
#include "test/fuzz/FuzzUtils.h"
#include "util/Math.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace stellar
{

FuzzTargetRegistry&
FuzzTargetRegistry::instance()
{
    static FuzzTargetRegistry registry;
    return registry;
}

void
FuzzTargetRegistry::registerTarget(std::string const& name,
                                   std::string const& description,
                                   TargetFactory factory)
{
    // Check for duplicate registration
    for (auto const& target : mTargets)
    {
        if (target.name == name)
        {
            throw std::runtime_error("Duplicate fuzz target registration: " +
                                     name);
        }
    }
    mTargets.push_back({name, description, std::move(factory)});
}

std::vector<FuzzTargetRegistry::TargetInfo> const&
FuzzTargetRegistry::targets() const
{
    return mTargets;
}

std::optional<FuzzTargetRegistry::TargetInfo>
FuzzTargetRegistry::findTarget(std::string const& name) const
{
    auto it = std::find_if(mTargets.begin(), mTargets.end(),
                           [&](auto const& t) { return t.name == name; });
    if (it != mTargets.end())
    {
        return *it;
    }
    return std::nullopt;
}

std::unique_ptr<FuzzTarget>
FuzzTargetRegistry::createTarget(std::string const& name) const
{
    auto info = findTarget(name);
    if (!info)
    {
        throw std::runtime_error("Unknown fuzz target: " + name);
    }
    return info->factory();
}

std::vector<std::string>
FuzzTargetRegistry::listTargetNames() const
{
    std::vector<std::string> names;
    names.reserve(mTargets.size());
    for (auto const& target : mTargets)
    {
        names.push_back(target.name);
    }
    return names;
}

void
FuzzTarget::generateSeedCorpus(std::string const& outputDir, size_t count)
{
    std::filesystem::create_directories(outputDir);

    // Initialize global state once before generating corpus
    reinitializeAllGlobalStateForFuzzing(std::random_device()());

    for (size_t i = 0; i < count; ++i)
    {
        auto data = generateSeedInput();

        auto filename = std::filesystem::path(outputDir) /
                        (name() + "_seed_" + std::to_string(i) + ".bin");

        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(filename, std::ofstream::binary | std::ofstream::trunc);
        out.write(reinterpret_cast<char const*>(data.data()), data.size());
    }
}

FuzzTargetRegistrar::FuzzTargetRegistrar(
    std::string const& name, std::string const& description,
    FuzzTargetRegistry::TargetFactory factory)
{
    FuzzTargetRegistry::instance().registerTarget(name, description,
                                                  std::move(factory));
}

} // namespace stellar
