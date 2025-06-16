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

#ifndef CRASHPAD_UTIL_MISC_ATTACHMENT_H_
#define CRASHPAD_UTIL_MISC_ATTACHMENT_H_

#include <vector>

#include "base/files/file_path.h"
#include "util/misc/uuid.h"

namespace crashpad {

class Attachment {
 public:
  Attachment(const base::FilePath& path, const UUID& uuid);
  Attachment(const std::vector<uint8_t>& bytes,
             const base::FilePath& filename,
             const UUID& uuid);

  static Attachment FromPath(const base::FilePath& path);
  static Attachment FromBytes(const std::vector<uint8_t>& bytes,
                              const base::FilePath& filename);

  bool operator==(const Attachment& other) const {
    return uuid_ == other.uuid_;
  }

  UUID GetUuid() const { return uuid_; }
  const base::FilePath& GetPath() const { return path_; }

  bool HasBytes() const { return !bytes_.empty(); }
  const std::vector<uint8_t>& GetBytes() const { return bytes_; }

 private:
  UUID uuid_;
  base::FilePath path_;
  std::vector<uint8_t> bytes_;
};

}  // namespace crashpad

#endif  // CRASHPAD_UTIL_MISC_ATTACHMENT_H_
