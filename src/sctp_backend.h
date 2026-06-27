#pragma once

#include "app_common.h"

namespace proto_test {

void RunSctpServer(const AppConfig& config);
void RunSctpClient(const AppConfig& config);

} // namespace proto_test
