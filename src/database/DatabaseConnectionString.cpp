// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "DatabaseConnectionString.h"

#include <regex>

namespace stellar
{
std::string
removePasswordFromConnectionString(std::string connectionString)
{
    std::string escapedSingleQuotePat("\\\\'");
    std::string nonSingleQuotePat("[^']");
    std::string singleQuotedStringPat("'(?:" + nonSingleQuotePat + "|" +
                                      escapedSingleQuotePat + ")*'");
    std::string bareWordPat("\\S+");
    std::string paramValPat("(?:" + bareWordPat + "|" + singleQuotedStringPat +
                            ")");
    std::string paramPat("(?:" + bareWordPat + " *= *" + paramValPat + " *)");
    std::string passwordParamPat("password( *= *)" + paramValPat);
    std::string connPat("^(" + bareWordPat + ":// *)(" + paramPat + "*)" +
                        passwordParamPat + "( *)(" + paramPat + "*)$");
    return std::regex_replace(connectionString, std::regex(connPat),
                              "$1$2password$3********$4$5");
}
}
