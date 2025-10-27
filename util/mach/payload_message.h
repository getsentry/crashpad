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

#ifndef CRASHPAD_UTIL_MACH_PAYLOAD_MESSAGE_H_
#define CRASHPAD_UTIL_MACH_PAYLOAD_MESSAGE_H_

#include <mach/mach.h>
#include <string>

namespace crashpad {

//! \brief Payload message type
enum PayloadMessageType { kAddAttachment = 1, kRemoveAttachment = 2 };

//! \brief Payload message ID
constexpr mach_msg_id_t kPayloadMessageID = 2404;

//! \brief Message structure with payload
struct PayloadMessage {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_ool_descriptor_t payload;
  NDR_record_t ndr;
  PayloadMessageType type;
};

bool SendPayloadMessage(mach_port_t port,
                        PayloadMessageType type,
                        const std::string& payload);

const PayloadMessage* ReceivePayloadMessage(const mach_msg_header_t* in_header,
                                            mach_msg_header_t* out_header);

}  // namespace crashpad

#endif  // CRASHPAD_UTIL_MACH_PAYLOAD_MESSAGE_H_
