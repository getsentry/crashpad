// Copyright 2015 The Crashpad Authors
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

#include "handler/crash_report_upload_thread.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/notreached.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "client/settings.h"
#include "handler/crash_report_upload_rate_limit.h"
#include "handler/minidump_to_upload_parameters.h"
#include "snapshot/minidump/process_snapshot_minidump.h"
#include "snapshot/module_snapshot.h"
#include "util/file/file_reader.h"
#include "util/file/string_file.h"
#include "util/misc/metrics.h"
#include "util/misc/uuid.h"
#include "util/net/http_body.h"
#include "util/net/http_headers.h"
#include "util/net/http_multipart_builder.h"
#include "util/net/http_transport.h"
#include "util/net/url.h"
#include "util/numeric/safe_assignment.h"
#include "util/stdlib/map_insert.h"

#if BUILDFLAG(IS_APPLE)
#include "handler/mac/file_limit_annotation.h"
#endif  // BUILDFLAG(IS_APPLE)

#if BUILDFLAG(IS_IOS)
#include "util/ios/scoped_background_task.h"
#endif  // BUILDFLAG(IS_IOS)

namespace crashpad {

namespace {

// The number of seconds to wait between checking for pending reports.
const int kRetryWorkIntervalSeconds = 15 * 60;

// The number of times to attempt to upload a pending report, repeated on
// failure. Attempts will happen once per launch, once per call to
// ReportPending(), and, if Options.watch_pending_reports is true, once every
// kRetryWorkIntervalSeconds.
const int kRetryAttempts = 5;

constexpr uint64_t kSentryLargeAttachmentSize = 100ull * 1024 * 1024;  // 100 MiB
constexpr uint64_t kSentryMaxAttachmentSize = 1024ull * 1024 * 1024;  // 1 GiB
constexpr char kTusResumable[] = "1.0.0";
constexpr char kTusMime[] = "application/offset+octet-stream";

struct LargeAttachmentUploadContext {
  std::string origin;
  std::string upload_url;
  std::string envelope_url;
  std::string auth_header;
};

struct UploadedLargeAttachment {
  std::string name;
  std::string location;
  uint64_t size;
};

bool StartsWith(const std::string& string, const char* prefix) {
  return string.compare(0, strlen(prefix), prefix) == 0;
}

bool EndsWith(const std::string& string, const char* suffix) {
  const size_t suffix_length = strlen(suffix);
  return string.size() >= suffix_length &&
         string.compare(string.size() - suffix_length, suffix_length, suffix) ==
             0;
}

std::string HeaderValue(const HTTPHeaders& headers, const char* name) {
  for (const auto& header : headers) {
    if (HTTPHeaderNameEquals(header.first, name)) {
      return header.second;
    }
  }
  return std::string();
}

std::string QueryValue(const std::string& query, const char* name) {
  size_t start = 0;
  while (start <= query.size()) {
    size_t end = query.find('&', start);
    if (end == std::string::npos) {
      end = query.size();
    }

    const std::string pair = query.substr(start, end - start);
    const size_t equals = pair.find('=');
    if (equals != std::string::npos && pair.compare(0, equals, name) == 0) {
      return pair.substr(equals + 1);
    }

    if (end == query.size()) {
      break;
    }
    start = end + 1;
  }
  return std::string();
}

bool BuildLargeAttachmentUploadContext(const std::string& minidump_url,
                                       LargeAttachmentUploadContext* context) {
  std::string scheme;
  std::string host;
  std::string port;
  std::string rest;
  if (!CrackURL(minidump_url, &scheme, &host, &port, &rest)) {
    return false;
  }

  const size_t query_separator = rest.find('?');
  const std::string path = rest.substr(0, query_separator);
  const std::string query = query_separator == std::string::npos
                                ? std::string()
                                : rest.substr(query_separator + 1);
  static constexpr char kMinidumpPathSuffix[] = "/minidump/";
  if (!EndsWith(path, kMinidumpPathSuffix)) {
    return false;
  }

  const std::string sentry_key = QueryValue(query, "sentry_key");
  if (sentry_key.empty()) {
    return false;
  }

  context->origin = base::StringPrintf(
      "%s://%s:%s", scheme.c_str(), host.c_str(), port.c_str());
  context->upload_url =
      context->origin +
      path.substr(0, path.size() - strlen(kMinidumpPathSuffix)) + "/upload/";
  context->envelope_url =
      context->origin +
      path.substr(0, path.size() - strlen(kMinidumpPathSuffix)) + "/envelope/";

  context->auth_header =
      "Sentry sentry_key=" + sentry_key + ", sentry_version=7";
  const std::string sentry_client = QueryValue(query, "sentry_client");
  if (!sentry_client.empty()) {
    context->auth_header += ", sentry_client=" + sentry_client;
  }

  return true;
}

std::string ResolveLargeAttachmentUploadLocation(
    const LargeAttachmentUploadContext& context,
    const std::string& location) {
  if (StartsWith(location, "http://") || StartsWith(location, "https://")) {
    return location;
  }
  if (!location.empty() && location[0] == '/') {
    return context.origin + location;
  }
  return location;
}

bool GetReaderSize(FileReaderInterface* reader, uint64_t* size) {
  const FileOffset current = reader->SeekGet();
  if (current < 0) {
    return false;
  }

  const FileOffset end = reader->Seek(0, SEEK_END);
  if (end < 0) {
    reader->SeekSet(current);
    return false;
  }

  const bool assigned = AssignIfInRange(size, end);
  if (!reader->SeekSet(current)) {
    return false;
  }
  return assigned;
}

bool CreateLargeAttachmentUpload(const LargeAttachmentUploadContext& context,
                                 const std::string& http_proxy,
                                 uint64_t upload_size,
                                 bool* upload_available,
                                 std::string* location) {
  std::unique_ptr<HTTPTransport> http_transport(HTTPTransport::Create());
  if (!http_transport) {
    return false;
  }

  http_transport->SetURL(context.upload_url);
  http_transport->SetHTTPProxy(http_proxy);
  http_transport->SetHeader("x-sentry-auth", context.auth_header);
  http_transport->SetHeader("tus-resumable", kTusResumable);
  http_transport->SetHeader("upload-length", std::to_string(upload_size));
  http_transport->SetHeader(kContentLength, "0");
  http_transport->SetBodyStream(std::make_unique<StringHTTPBodyStream>(""));
  http_transport->SetExpectedResponseCode(201);
  http_transport->SetTimeout(internal::kUploadReportTimeoutSeconds);

  std::string response_body;
  if (!http_transport->ExecuteSynchronously(&response_body)) {
    if (http_transport->response_code() == 404) {
      LOG(WARNING) << "large attachment upload endpoint returned 404, "
                      "disabling separate upload";
      *upload_available = false;
    }
    return false;
  }

  *location = HeaderValue(http_transport->response_headers(), "Location");
  if (location->empty()) {
    LOG(WARNING) << "large attachment upload response did not include Location";
    return false;
  }
  return true;
}

bool UploadLargeAttachmentBytes(const LargeAttachmentUploadContext& context,
                                const std::string& http_proxy,
                                FileReaderInterface* reader,
                                uint64_t upload_size,
                                const std::string& location) {
  if (!reader->SeekSet(0)) {
    return false;
  }

  std::unique_ptr<HTTPTransport> http_transport(HTTPTransport::Create());
  if (!http_transport) {
    return false;
  }

  http_transport->SetURL(
      ResolveLargeAttachmentUploadLocation(context, location));
  http_transport->SetHTTPProxy(http_proxy);
  http_transport->SetMethod("PATCH");
  http_transport->SetHeader("x-sentry-auth", context.auth_header);
  http_transport->SetHeader("tus-resumable", kTusResumable);
  http_transport->SetHeader(kContentType, kTusMime);
  http_transport->SetHeader("upload-offset", "0");
  http_transport->SetHeader(kContentLength, std::to_string(upload_size));
  http_transport->SetBodyStream(
      std::make_unique<FileReaderHTTPBodyStream>(reader));
  http_transport->SetExpectedResponseCode(204);
  http_transport->SetTimeout(internal::kUploadReportTimeoutSeconds);

  std::string response_body;
  const bool ok = http_transport->ExecuteSynchronously(&response_body);
  reader->SeekSet(0);
  return ok;
}

bool UploadLargeAttachment(const LargeAttachmentUploadContext& context,
                           const std::string& http_proxy,
                           const std::string& attachment_name,
                           FileReaderInterface* reader,
                           uint64_t upload_size,
                           bool* upload_available,
                           std::string* location) {
  if (!CreateLargeAttachmentUpload(
          context, http_proxy, upload_size, upload_available, location)) {
    return false;
  }

  if (!UploadLargeAttachmentBytes(
          context, http_proxy, reader, upload_size, *location)) {
    LOG(WARNING) << "large attachment upload failed for " << attachment_name;
    location->clear();
    reader->SeekSet(0);
    return false;
  }

  return true;
}

bool UploadLargeAttachmentEnvelope(const LargeAttachmentUploadContext& context,
                                   const std::string& http_proxy,
                                   const StringFile& envelope,
                                   std::string* response_body) {
  std::unique_ptr<HTTPTransport> http_transport(HTTPTransport::Create());
  if (!http_transport) {
    return false;
  }

  http_transport->SetURL(context.envelope_url);
  http_transport->SetHTTPProxy(http_proxy);
  http_transport->SetHeader("x-sentry-auth", context.auth_header);
  http_transport->SetHeader(kContentType, "application/x-sentry-envelope");
  http_transport->SetHeader(kContentLength,
                            std::to_string(envelope.string().size()));
  http_transport->SetBodyStream(
      std::make_unique<StringHTTPBodyStream>(envelope.string()));
  http_transport->SetTimeout(internal::kUploadReportTimeoutSeconds);

  return http_transport->ExecuteSynchronously(response_body);
}

// Wraps a reference to a no-args function (which can be empty). When this
// object goes out of scope, invokes the function if it is non-empty.
//
// The lifetime of the function must outlive the lifetime of this object.
class ScopedFunctionInvoker final {
 public:
  ScopedFunctionInvoker(const std::function<void()>& function)
      : function_(function) {}
  ScopedFunctionInvoker(const ScopedFunctionInvoker&) = delete;
  ScopedFunctionInvoker& operator=(const ScopedFunctionInvoker&) = delete;

  ~ScopedFunctionInvoker() {
    if (function_) {
      function_();
    }
  }

 private:
  const std::function<void()>& function_;
};

}  // namespace

CrashReportUploadThread::CrashReportUploadThread(
    CrashReportDatabase* database,
    std::string url,
    std::string http_proxy,
    const Options& options,
    ProcessPendingReportsObservationCallback callback)
    : options_(options),
      callback_(std::move(callback)),
      url_(std::move(url)),
      http_proxy_(std::move(http_proxy)),
      // When watching for pending reports, check every 15 minutes, even in the
      // absence of a signal from the handler thread. This allows for failed
      // uploads to be retried periodically, and for pending reports written by
      // other processes to be recognized.
      thread_(options.watch_pending_reports ? kRetryWorkIntervalSeconds
                                            : WorkerThread::kIndefiniteWait,
              this),
      known_pending_report_uuids_(),
      database_(database) {
  DCHECK(!url_.empty());
}

CrashReportUploadThread::~CrashReportUploadThread() {
}

void CrashReportUploadThread::ReportPending(const UUID& report_uuid) {
  known_pending_report_uuids_.PushBack(report_uuid);
  if (thread_.is_running())
    thread_.DoWorkNow();
}

void CrashReportUploadThread::ReportPendingSync(const UUID& report_uuid) {
  known_pending_report_uuids_.PushBack(report_uuid);
  DoWork(nullptr);
}

void CrashReportUploadThread::RetryPending() {
  if (thread_.is_running())
    thread_.DoWorkNow();
}

void CrashReportUploadThread::Start() {
  thread_.Start(
      options_.watch_pending_reports ? 0.0 : WorkerThread::kIndefiniteWait);
}

void CrashReportUploadThread::Stop() {
  thread_.Stop();
}

void CrashReportUploadThread::ProcessPendingReports() {
#if BUILDFLAG(IS_IOS)
  internal::ScopedBackgroundTask scoper("CrashReportUploadThread");
#endif  // BUILDFLAG(IS_IOS)

  // If callback_ is non-empty, invoke it when this function returns after
  // uploads complete (regardless of whether or not that succeeded).
  ScopedFunctionInvoker scoped_function_invoker(callback_);

  bool uploads_paused;
  if (database_->GetSettings()->GetUploadsPaused(&uploads_paused) &&
      uploads_paused) {
    // Leave known pending report UUIDs in the queue so they are retried once
    // the pause is lifted, and skip scanning for new pending reports.
    return;
  }

  std::vector<UUID> known_report_uuids = known_pending_report_uuids_.Drain();
  for (const UUID& report_uuid : known_report_uuids) {
    CrashReportDatabase::Report report;
    if (database_->LookUpCrashReport(report_uuid, &report) !=
        CrashReportDatabase::kNoError) {
      continue;
    }

    ProcessPendingReport(report);

    // Respect Stop() being called after at least one attempt to process a
    // report.
    if (!thread_.is_running()) {
      return;
    }
  }

  // Known pending reports are always processed (above). The rest of this
  // function is concerned with scanning for pending reports not already known
  // to this thread.
  if (!options_.watch_pending_reports) {
    return;
  }

  std::vector<CrashReportDatabase::Report> reports;
  if (database_->GetPendingReports(&reports) != CrashReportDatabase::kNoError) {
    // The database is sick. It might be prudent to stop trying to poke it from
    // this thread by abandoning the thread altogether. On the other hand, if
    // the problem is transient, it might be possible to talk to it again on the
    // next pass. For now, take the latter approach.
    return;
  }

  for (const CrashReportDatabase::Report& report : reports) {
    if (std::find(known_report_uuids.begin(),
                  known_report_uuids.end(),
                  report.uuid) != known_report_uuids.end()) {
      // An attempt to process the report already occurred above. The report is
      // still pending, so upload must have failed. Don’t retry it immediately,
      // it can wait until at least the next pass through this method.
      continue;
    }

    ProcessPendingReport(report);

    // Respect Stop() being called after at least one attempt to process a
    // report.
    if (!thread_.is_running()) {
      return;
    }
  }
}

void CrashReportUploadThread::ProcessPendingReport(
    const CrashReportDatabase::Report& report) {
#if BUILDFLAG(IS_APPLE)
  RecordFileLimitAnnotation();
#endif  // BUILDFLAG(IS_APPLE)

  Settings* const settings = database_->GetSettings();

  bool uploads_enabled;
  if (!report.upload_explicitly_requested &&
      (!settings->GetUploadsEnabled(&uploads_enabled) || !uploads_enabled)) {
    // Don’t attempt an upload if there’s no URL to upload to. Allow upload if
    // it has been explicitly requested by the user, otherwise, respect the
    // upload-enabled state stored in the database’s settings.
    database_->SkipReportUpload(report.uuid,
                                Metrics::CrashSkippedReason::kUploadsDisabled);
    return;
  }

  if (ShouldRateLimitUpload(report))
    return;

  if (ShouldRateLimitRetry(report))
    return;

  std::unique_ptr<const CrashReportDatabase::UploadReport> upload_report;
  CrashReportDatabase::OperationStatus status =
      database_->GetReportForUploading(report.uuid, &upload_report);
  switch (status) {
    case CrashReportDatabase::kNoError:
      break;

    case CrashReportDatabase::kBusyError:
    case CrashReportDatabase::kReportNotFound:
      // Someone else may have gotten to it first. If they’re working on it now,
      // this will be kBusyError. If they’ve already finished with it, it’ll be
      // kReportNotFound.
      return;

    case CrashReportDatabase::kFileSystemError:
    case CrashReportDatabase::kDatabaseError:
      // In these cases, SkipReportUpload() might not work either, but it’s best
      // to at least try to get the report out of the way.
      database_->SkipReportUpload(report.uuid,
                                  Metrics::CrashSkippedReason::kDatabaseError);
      return;

    case CrashReportDatabase::kCannotRequestUpload:
      NOTREACHED();
  }

  std::string response_body;
  UploadResult upload_result =
      UploadReport(upload_report.get(), &response_body);
  switch (upload_result) {
    case UploadResult::kSuccess:
      database_->RecordUploadComplete(std::move(upload_report), response_body);
      break;
    case UploadResult::kPermanentFailure:
      upload_report.reset();
      database_->SkipReportUpload(
          report.uuid, Metrics::CrashSkippedReason::kPrepareForUploadFailed);
      break;
    case UploadResult::kRetry:
      if (upload_report->upload_attempts > kRetryAttempts) {
        upload_report.reset();
        database_->SkipReportUpload(report.uuid,
                                    Metrics::CrashSkippedReason::kUploadFailed);
      } else {
        Metrics::CrashUploadSkipped(
            Metrics::CrashSkippedReason::kUploadFailedButCanRetry);
        retry_uuid_time_map_[report.uuid] =
            time(nullptr) +
            (1 << upload_report->upload_attempts) * kRetryWorkIntervalSeconds;
      }
      break;
  }
}

CrashReportUploadThread::UploadResult CrashReportUploadThread::UploadReport(
    const CrashReportDatabase::UploadReport* report,
    std::string* response_body) {
  std::map<std::string, std::string> parameters;

  FileReader* reader = report->Reader();
  FileOffset start_offset = reader->SeekGet();
  if (start_offset < 0) {
    return UploadResult::kPermanentFailure;
  }

  // Ignore any errors that might occur when attempting to interpret the
  // minidump file. This may result in its being uploaded with few or no
  // parameters, but as long as there’s a dump file, the server can decide what
  // to do with it.
  ProcessSnapshotMinidump minidump_process_snapshot;
  if (minidump_process_snapshot.Initialize(reader)) {
    parameters =
        BreakpadHTTPFormParametersFromMinidump(&minidump_process_snapshot);
  }

  if (!reader->SeekSet(start_offset)) {
    return UploadResult::kPermanentFailure;
  }

  static constexpr char kMinidumpKey[] = "upload_file_minidump";

  LargeAttachmentUploadContext large_attachment_upload_context;
  bool large_attachment_upload_available =
      options_.enable_large_attachments &&
      BuildLargeAttachmentUploadContext(url_, &large_attachment_upload_context);
  if (options_.enable_large_attachments && !large_attachment_upload_available) {
    LOG(WARNING) << "large attachments enabled, but large attachment upload "
                    "context could not be derived from the report URL";
  }

  std::map<std::string, FileReader*> attachments = report->GetAttachments();
  std::vector<UploadedLargeAttachment> uploaded_attachments;
  for (auto it = attachments.begin(); it != attachments.end();) {
    if (options_.enable_large_attachments) {
      uint64_t attachment_size = 0;
      if (!GetReaderSize(it->second, &attachment_size)) {
        return UploadResult::kPermanentFailure;
      }

      if (attachment_size > kSentryMaxAttachmentSize) {
        LOG(WARNING) << "discarding attachment " << it->first
                     << " because it exceeds the maximum attachment size";
        it = attachments.erase(it);
        continue;
      }

      if (large_attachment_upload_available &&
          attachment_size >= kSentryLargeAttachmentSize) {
        std::string location;
        if (UploadLargeAttachment(large_attachment_upload_context,
                                  http_proxy_,
                                  it->first,
                                  it->second,
                                  attachment_size,
                                  &large_attachment_upload_available,
                                  &location)) {
          uploaded_attachments.push_back(
              {it->first, location, attachment_size});
          it = attachments.erase(it);
          continue;
        }
      }
    }

    ++it;
  }

  const std::string minidump_name = report->uuid.ToString() + ".dmp";
  bool minidump_uploaded_separately = false;
  uint64_t minidump_size = 0;
  std::string minidump_location;
  if (options_.enable_large_attachments) {
    if (!GetReaderSize(reader, &minidump_size)) {
      return UploadResult::kPermanentFailure;
    }

    if (large_attachment_upload_available &&
        minidump_size >= kSentryLargeAttachmentSize &&
        minidump_size <= kSentryMaxAttachmentSize) {
      std::string location;
      if (UploadLargeAttachment(large_attachment_upload_context,
                                http_proxy_,
                                minidump_name,
                                reader,
                                minidump_size,
                                &large_attachment_upload_available,
                                &minidump_location)) {
        minidump_uploaded_separately = true;
      }
    }
  }

  if (minidump_uploaded_separately || !uploaded_attachments.empty()) {
    StringFile envelope_file;
    CrashReportDatabase::Envelope envelope(report->uuid);
    if (!envelope.Initialize(&envelope_file)) {
      return UploadResult::kPermanentFailure;
    }

    envelope.AddAttachments(attachments);
    if (minidump_uploaded_separately) {
      envelope.AddMinidumpRef(minidump_name, minidump_location, minidump_size);
    } else {
      envelope.AddMinidump(reader);
    }
    for (const auto& attachment : uploaded_attachments) {
      envelope.AddAttachmentRef(
          attachment.name, attachment.location, attachment.size);
    }
    envelope.Finish();

    if (!UploadLargeAttachmentEnvelope(large_attachment_upload_context,
                                       http_proxy_,
                                       envelope_file,
                                       response_body)) {
      return UploadResult::kRetry;
    }

    return UploadResult::kSuccess;
  }

  HTTPMultipartBuilder http_multipart_builder;
  http_multipart_builder.SetGzipEnabled(options_.upload_gzip);

  for (const auto& kv : parameters) {
    if (kv.first == kMinidumpKey) {
      LOG(WARNING) << "reserved key " << kv.first << ", discarding value "
                   << kv.second;
    } else {
      http_multipart_builder.SetFormData(kv.first, kv.second);
    }
  }

  for (const auto& it : attachments) {
    http_multipart_builder.SetFileAttachment(
        it.first, it.first, it.second, "application/octet-stream");
  }

  http_multipart_builder.SetFileAttachment(kMinidumpKey,
                                           minidump_name,
                                           reader,
                                           "application/octet-stream");

  std::unique_ptr<HTTPTransport> http_transport(HTTPTransport::Create());
  if (!http_transport) {
    return UploadResult::kPermanentFailure;
  }

  HTTPHeaders content_headers;
  http_multipart_builder.PopulateContentHeaders(&content_headers);
  for (const auto& content_header : content_headers) {
    http_transport->SetHeader(content_header.first, content_header.second);
  }
  http_transport->SetBodyStream(http_multipart_builder.GetBodyStream());
  // TODO(mark): The timeout should be configurable by the client.
  http_transport->SetTimeout(internal::kUploadReportTimeoutSeconds);

  std::string url = url_;
  if (options_.identify_client_via_url) {
    // Add parameters to the URL which identify the client to the server.
    static constexpr struct {
      const char* key;
      const char* url_field_name;
    } kURLParameterMappings[] = {
        {"prod", "product"},
        {"ver", "version"},
        {"guid", "guid"},
    };

    for (const auto& parameter_mapping : kURLParameterMappings) {
      const auto it = parameters.find(parameter_mapping.key);
      if (it != parameters.end()) {
        url.append(
            base::StringPrintf("%c%s=%s",
                               url.find('?') == std::string::npos ? '?' : '&',
                               parameter_mapping.url_field_name,
                               URLEncode(it->second).c_str()));
      }
    }
  }
  http_transport->SetURL(url);
  http_transport->SetHTTPProxy(http_proxy_);

  if (!http_transport->ExecuteSynchronously(response_body)) {
    return UploadResult::kRetry;
  }

  return UploadResult::kSuccess;
}

void CrashReportUploadThread::DoWork(const WorkerThread* thread) {
  ProcessPendingReports();
}

bool CrashReportUploadThread::ShouldRateLimitUpload(
    const CrashReportDatabase::Report& report) {
  if (report.upload_explicitly_requested || !options_.rate_limit)
    return false;

  Settings* const settings = database_->GetSettings();
  time_t last_upload_attempt_time;
  if (settings->GetLastUploadAttemptTime(&last_upload_attempt_time)) {
    const time_t now = time(nullptr);
    constexpr int kUploadAttemptIntervalSeconds = 60 * 60;  // 1 hour
    const auto should_rate_limit = ShouldRateLimit(
        now, last_upload_attempt_time, kUploadAttemptIntervalSeconds);
    if (should_rate_limit.skip_reason.has_value()) {
      database_->SkipReportUpload(report.uuid, *should_rate_limit.skip_reason);
      return true;
    }
  }
  return false;
}

bool CrashReportUploadThread::ShouldRateLimitRetry(
    const CrashReportDatabase::Report& report) {
  if (retry_uuid_time_map_.find(report.uuid) != retry_uuid_time_map_.end()) {
    time_t now = time(nullptr);
    if (now < retry_uuid_time_map_[report.uuid]) {
      return true;
    } else {
      retry_uuid_time_map_.erase(report.uuid);
    }
  }
  return false;
}

}  // namespace crashpad
