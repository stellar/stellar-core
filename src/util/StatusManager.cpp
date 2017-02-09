// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "StatusManager.h"

#include <array>

namespace stellar
{

StatusManager::StatusManager()
{
}

StatusManager::~StatusManager()
{
}

void
StatusManager::setStatusMessage(StatusCategory issue, std::string message)
{
    mStatusMessages[issue] = std::move(message);
}

void
StatusManager::removeStatusMessage(StatusCategory issue)
{
    mStatusMessages.erase(issue);
}

std::string
StatusManager::getStatusMessage(StatusCategory issue) const
{
    auto it = mStatusMessages.find(issue);
    if (it == mStatusMessages.end())
    {
        return std::string{};
    }
    else
    {
        return it->second;
    }
}
}
