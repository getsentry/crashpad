// Copyright 2014 The Crashpad Authors
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

#include "util/misc/attachment.h"

namespace crashpad {

Attachment::Attachment(const base::FilePath& path, const UUID& uuid)
    : uuid_(uuid), path_(path) {}

Attachment::Attachment(const std::vector<uint8_t>& bytes,
                       const base::FilePath& filename,
                       const UUID& uuid)
    : uuid_(uuid), path_(filename), bytes_(bytes) {}

Attachment Attachment::FromPath(const base::FilePath& path) {
  UUID uuid;
  uuid.InitializeWithNew();
  return Attachment(path, uuid);
}

Attachment Attachment::FromBytes(const std::vector<uint8_t>& bytes,
                                 const base::FilePath& filename) {
  UUID uuid;
  uuid.InitializeWithNew();
  return Attachment(bytes, filename, uuid);
}

}  // namespace crashpad
