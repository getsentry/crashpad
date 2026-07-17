// Copyright 2026 The Crashpad Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "client/crashpad_client.h"

#include <map>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "build/build_config.h"

int main() {
#if BUILDFLAG(IS_IOS)
  crashpad::CrashpadClient::StartCrashpadInProcessHandler(
      base::FilePath(),
      std::string(),
      std::map<std::string, std::string>(),
      crashpad::CrashpadClient::ProcessPendingReportsObservationCallback());
#else
  crashpad::CrashpadClient client;
  client.StartHandler(base::FilePath(),
                      base::FilePath(),
                      base::FilePath(),
                      std::string(),
                      std::string(),
                      std::map<std::string, std::string>(),
                      std::vector<std::string>(),
                      false,
                      false);
#endif
}
