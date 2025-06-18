// Copyright 2020 The Crashpad Authors
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

#ifndef CRASHPAD_UTIL_FILE_FILE_HELPER_H_
#define CRASHPAD_UTIL_FILE_FILE_HELPER_H_

#include "base/files/file_path.h"
#include "util/file/file_reader.h"
#include "util/file/file_writer.h"

namespace crashpad {

//! \brief Copy the file content from file_reader to file_writer
void CopyFileContent(FileReaderInterface* file_reader,
                     FileWriterInterface* file_writer);

//! \brief Ensure that the given file path is unique.
//
// If the file exists, "-N" is appended to the base name before the extension,
// where N is a number starting from 1.
base::FilePath EnsureUniqueFile(const base::FilePath& path);

}  // namespace crashpad

#endif  // CRASHPAD_UTIL_FILE_FILE_HELPER_H_
