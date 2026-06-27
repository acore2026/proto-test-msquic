#pragma once

#include "app_common.h"

namespace proto_test {

void RunMsQuicServer(const AppConfig& config);
void RunMsQuicClient(const AppConfig& config);

} // namespace proto_test
