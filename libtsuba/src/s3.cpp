#include "s3.h"

#include <unistd.h>

#include <algorithm>
#include <memory>
#include <stack>
#include <string_view>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/stream/PreallocatedStreamBuf.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/Delete.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/ObjectIdentifier.h>
#include <aws/transfer/TransferManager.h>
#include <fmt/core.h>

#include "SegmentedBufferView.h"
#include "galois/FileSystem.h"
#include "galois/GetEnv.h"
#include "galois/Logging.h"
#include "galois/Result.h"
#include "tsuba/Errors.h"
#include "tsuba/FaultTest.h"
#include "tsuba/s3_internal.h"
#include "tsuba_internal.h"

namespace tsuba {

static Aws::SDKOptions sdk_options;

static constexpr const char* kDefaultS3Region = "us-east-1";
static constexpr const char* kAwsTag = "TsubaS3Client";
// TODO: Could explore policies
// Limits come from here.
//   https://docs.aws.amazon.com/AmazonS3/latest/dev/qfacts.html
// We use these defaults (from aws s3 cli)
//   https://docs.aws.amazon.com/cli/latest/topic/s3-config.html
static constexpr const uint64_t kS3MinBufSize = MB(5);
static constexpr const uint64_t kS3DefaultBufSize = MB(8);
static constexpr const uint64_t kS3MaxBufSize = GB(5);
static constexpr const uint64_t kS3MaxMultiPart = 10000;
// Can only delete 1000 objects at a time, but we are conservative
static constexpr const uint64_t kS3MaxDelete = 995;
// TODO: How to set this number?  Base it on cores on machines and/or memory
// use?
static constexpr const uint64_t kNumS3Threads = 36;

// Initialized in S3Init
static std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor>
    default_executor{nullptr};
static std::shared_ptr<Aws::S3::S3Client> async_s3_client{nullptr};
static bool library_init{false};

class WarnOnEmptyDefaultCredentialsChain
    : public Aws::Auth::DefaultAWSCredentialsProviderChain {
public:
  WarnOnEmptyDefaultCredentialsChain()
      : Aws::Auth::DefaultAWSCredentialsProviderChain() {}
  Aws::Auth::AWSCredentials GetAWSCredentials() override {
    auto creds =
        Aws::Auth::DefaultAWSCredentialsProviderChain::GetAWSCredentials();
    if (creds.IsEmpty()) {
      bool metadata_disabled = false;
      galois::GetEnv("AWS_EC2_METADATA_DISABLED", &metadata_disabled);

      if (!metadata_disabled) {
        GALOIS_WARN_ONCE(
            "AWS credentials not found. S3 storage will likely be\n"
            "    inaccessible. Not providing credentials can slow\n"
            "    initialization down considerably. If you don't\n"
            "    intend to use S3 you can set\n"
            "    \"AWS_EC2_METADATA_DISABLED=true\" in the environment\n"
            "    to bypass the most expensive check.");
      }
    }
    return creds;
  }
};

/// GetS3Client returns a configured S3 client.
///
/// The client pulls its configuration from the environment using the same
/// environment variables and configuration paths as the AWS CLI, although with
/// a subset of the configurability.
///
/// The AWS region is determined by:
///
/// 1. The region associated with the default profile in $HOME/.aws/config The
///    location configuration file can be overriden by env[AWS_CONFIG_FILE].
/// 2. Otherwise, env[AWS_DEFAULT_REGION]
/// 3. Otherwise, us-east-1
///
/// The credentials are determined by (in order of precedence):
///
/// 1. The environment variables AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY
/// 2. The credentials associcated with the default profile in
/// $HOME/.aws/credentials
/// 3. An external credential provider command as noted in the config file.
/// 4. STS assume role credenials
/// https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRole.html
/// 5. IAM roles for tasks (containers)
/// https://docs.aws.amazon.com/AmazonECS/latest/developerguide/task-iam-roles.html
/// 6. The machine's account (via EC2 metadata service) if on EC2.
static inline std::shared_ptr<Aws::S3::S3Client>
GetS3Client(const std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor>&
                executor) {
  Aws::Client::ClientConfiguration cfg("default");

  bool use_virtual_addressing = true;

  std::string region;
  if (galois::GetEnv("AWS_DEFAULT_REGION", &region)) {
    cfg.region = region;
  }

  if (cfg.region.empty()) {
    // The AWS SDK says the default region is us-east-1 but it appears we need
    // to set it ourselves.
    cfg.region = kDefaultS3Region;
  }

  cfg.executor = executor;

  std::string test_endpoint;
  // No official AWS environment analog so use GALOIS prefix
  galois::GetEnv("GALOIS_AWS_TEST_ENDPOINT", &test_endpoint);
  if (!test_endpoint.empty()) {
    cfg.endpointOverride = test_endpoint;
    cfg.scheme = Aws::Http::Scheme::HTTP;

    // if false SDK will build "path-style" URLs if true the URLs will be
    // "virtual-host-style" URLs LocalStack only supports the former but they
    // are deprecated for new buckets in s3
    use_virtual_addressing = false;
  }

  return Aws::MakeShared<Aws::S3::S3Client>(
      kAwsTag, Aws::MakeShared<WarnOnEmptyDefaultCredentialsChain>(kAwsTag),
      cfg, Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
      use_virtual_addressing);
}

static inline std::shared_ptr<Aws::S3::S3Client>
GetS3Client() {
  GALOIS_LOG_VASSERT(
      library_init == true, "Must call tsuba::Init before S3 interaction");
  return GetS3Client(default_executor);
}

template <class OutcomeType>
static galois::Result<void>
CheckS3Error(const OutcomeType& outcome) {
  if (outcome.IsSuccess()) {
    return galois::ResultSuccess();
  }
  const auto& error = outcome.GetError();
  if (error.GetResponseCode() ==
      Aws::Http::HttpResponseCode::MOVED_PERMANENTLY) {
    return ErrorCode::AWSWrongRegion;
  }
  return ErrorCode::S3Error;
}

static SegmentedBufferView
SegmentBuf(uint64_t start, const uint8_t* data, uint64_t size) {
  uint64_t segment_size = kS3DefaultBufSize;
  if ((size / kS3DefaultBufSize) > kS3MaxMultiPart) {
    // I can't find anything that says this needs to be an "even" number
    // Add one because integer arithmetic is floor
    segment_size = size / (kS3MaxMultiPart + 1);
    GALOIS_LOG_VASSERT(
        (segment_size > kS3MinBufSize) && (segment_size < kS3MaxBufSize),
        "\n  Min {:d} Max {:d} Default {:d} Request (too big) {:d} Segment "
        "{:d}",
        kS3MinBufSize, kS3MaxBufSize, kS3DefaultBufSize, size, segment_size);
  }
  return SegmentedBufferView(
      start, const_cast<uint8_t*>(data), size, segment_size);
}

struct internal::PutMultiImpl {
  std::vector<SegmentedBufferView::BufPart> parts_{};
  std::future<Aws::S3::Model::CreateMultipartUploadOutcome> create_fut_{};
  std::future<Aws::S3::Model::CompleteMultipartUploadOutcome> outcome_fut_{};
  std::vector<std::string> part_e_tags_{};
  internal::CountingSemaphore sema{};
  std::string upload_id_{""};
  // Just a container
  PutMultiImpl() {}
};

galois::Result<void>
S3Init() {
  library_init = true;
  Aws::InitAPI(sdk_options);
  default_executor =
      Aws::MakeShared<Aws::Utils::Threading::PooledThreadExecutor>(
          kAwsTag, kNumS3Threads);
  // Need context for async uploads
  async_s3_client = GetS3Client();
  return galois::ResultSuccess();
}

galois::Result<void>
S3Fini() {
  Aws::ShutdownAPI(sdk_options);
  return galois::ResultSuccess();
}

galois::Result<void>
S3GetSize(
    const std::string& bucket, const std::string& object, uint64_t* size) {
  auto s3_client = GetS3Client();
  /* skip all of the thread management overhead if we only have one request */
  Aws::S3::Model::HeadObjectRequest request;
  request.SetBucket(ToAwsString(bucket));
  request.SetKey(ToAwsString(object));
  Aws::S3::Model::HeadObjectOutcome outcome = s3_client->HeadObject(request);

  if (auto res = CheckS3Error(outcome); !res) {
    if (res.error() == ErrorCode::S3Error) {
      // This is S3Stat, which can legitimately be called on a non-existent file
      GALOIS_LOG_DEBUG(
          "S3GetSize\n  [{}] {}\n  {}: {} {}\n", bucket, object,
          outcome.GetError().GetResponseCode(),
          outcome.GetError().GetExceptionName(),
          outcome.GetError().GetMessage());
    }
    return res;
  }
  *size = outcome.GetResult().GetContentLength();
  return galois::ResultSuccess();
}

/// Return 1 if bucket/object exists, 0 otherwise
galois::Result<bool>
S3Exists(const std::string& bucket, const std::string& object) {
  auto s3_client = GetS3Client();
  /* skip all of the thread management overhead if we only have one request */
  Aws::S3::Model::HeadObjectRequest request;
  request.SetBucket(ToAwsString(bucket));
  request.SetKey(ToAwsString(object));
  Aws::S3::Model::HeadObjectOutcome outcome = s3_client->HeadObject(request);
  if (!outcome.IsSuccess()) {
    return false;
  }
  return true;
}

galois::Result<void>
internal::S3PutSingleSync(
    const std::string& bucket, const std::string& object, const uint8_t* data,
    uint64_t size) {
  auto s3_client = GetS3Client();
  Aws::S3::Model::PutObjectRequest object_request;

  object_request.SetBucket(ToAwsString(bucket));
  object_request.SetKey(ToAwsString(object));
  auto streamBuf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>(
      kAwsTag, (uint8_t*)data, static_cast<size_t>(size));
  auto preallocatedStreamReader =
      Aws::MakeShared<Aws::IOStream>(kAwsTag, streamBuf);

  object_request.SetBody(preallocatedStreamReader);
  object_request.SetContentType("application/octet-stream");

  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  auto outcome = s3_client->PutObject(object_request);
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  if (!outcome.IsSuccess()) {
    /* TODO there are likely some errors we can handle gracefully
     * i.e., with retries */
    const auto& error = outcome.GetError();
    GALOIS_LOG_ERROR(
        "\n  Upload failed: {}: {}\n  [{}] {}", error.GetExceptionName(),
        error.GetMessage(), bucket, object);
    return ErrorCode::S3Error;
  }
  return galois::ResultSuccess();
}

galois::Result<void>
S3UploadOverwrite(
    const std::string& bucket, const std::string& object, const uint8_t* data,
    uint64_t size) {
  // Any small size put, do synchronously
  if (size < kS3DefaultBufSize) {
    GALOIS_LOG_DEBUG(
        "S3 Put {:d} bytes, less than {:d}, doing sync", size,
        kS3DefaultBufSize);
    return internal::S3PutSingleSync(bucket, object, data, size);
  }

  auto s3_client = GetS3Client();

  Aws::S3::Model::CreateMultipartUploadRequest createMpRequest;
  createMpRequest.WithBucket(ToAwsString(bucket));
  createMpRequest.WithContentType("application/octet-stream");
  createMpRequest.WithKey(ToAwsString(object));

  auto createMpResponse = s3_client->CreateMultipartUpload(createMpRequest);
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  if (auto res = CheckS3Error(createMpResponse); !res) {
    if (res.error() == ErrorCode::S3Error) {
      const auto& error = createMpResponse.GetError();
      GALOIS_LOG_ERROR(
          "Transfer failed to create a multi-part upload request\n"
          "  [{}] {}\n  {}: {}\n",
          bucket, object, error.GetExceptionName(), error.GetMessage());
    }
    return res.error();
  }

  auto upload_id = createMpResponse.GetResult().GetUploadId();
  SegmentedBufferView bufView = SegmentBuf(UINT64_C(0), data, size);
  std::vector<SegmentedBufferView::BufPart> parts(
      bufView.begin(), bufView.end());
  // Because zero-length upload handled above, parts should not be empty
  GALOIS_LOG_ASSERT(!parts.empty());

  std::vector<std::string> part_e_tags(parts.size());

  std::mutex m;
  std::condition_variable cv;
  Aws::S3::Model::CompletedMultipartUpload completedUpload;
  uint64_t finished = 0;
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  for (unsigned i = 0; i < parts.size(); ++i) {
    auto& part = parts[i];
    auto lengthToWrite = part.end - part.start;
    auto streamBuf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>(
        kAwsTag, part.dest, static_cast<size_t>(lengthToWrite));
    auto preallocatedStreamReader =
        Aws::MakeShared<Aws::IOStream>(kAwsTag, streamBuf);
    Aws::S3::Model::UploadPartRequest uploadPartRequest;

    uploadPartRequest.WithBucket(ToAwsString(bucket))
        .WithContentLength(static_cast<long long>(lengthToWrite))
        .WithKey(ToAwsString(object))
        .WithPartNumber(i + 1) /* part numbers start at 1 */
        .WithUploadId(upload_id);

    uploadPartRequest.SetBody(preallocatedStreamReader);
    uploadPartRequest.SetContentType("application/octet-stream");
    auto callback =
        [i, bucket, object, &part_e_tags, &cv, &m, &finished](
            const Aws::S3::S3Client* client,
            const Aws::S3::Model::UploadPartRequest& request,
            const Aws::S3::Model::UploadPartOutcome& outcome,
            const std::shared_ptr<const Aws::Client::AsyncCallerContext>&
                context) {
          /* we're not using these but they need to be here to preserve the
           * signature
           */
          (void)(client);
          (void)(request);
          (void)(context);
          TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
          if (outcome.IsSuccess()) {
            std::lock_guard<std::mutex> lk(m);
            TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
            part_e_tags[i] = outcome.GetResult().GetETag();
            finished++;
            cv.notify_one();
            TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
          } else {
            const auto& error = outcome.GetError();
            GALOIS_LOG_FATAL(
                "Upload multi callback failure\n  {}: {}\n  [{}] {}",
                error.GetExceptionName(), error.GetMessage(), bucket, object);
          }
        };
    s3_client->UploadPartAsync(uploadPartRequest, callback);
    TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  }
  std::unique_lock<std::mutex> lk(m);
  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  cv.wait(lk, [&] {
    TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
    return finished >= parts.size();
  });

  for (unsigned i = 0; i < part_e_tags.size(); ++i) {
    Aws::S3::Model::CompletedPart completedPart;
    completedPart.WithPartNumber(i + 1).WithETag(ToAwsString(part_e_tags[i]));
    completedUpload.AddParts(completedPart);
    TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  }

  Aws::S3::Model::CompleteMultipartUploadRequest completeMultipartUploadRequest;
  completeMultipartUploadRequest.WithBucket(ToAwsString(bucket))
      .WithKey(ToAwsString(object))
      .WithUploadId(upload_id)
      .WithMultipartUpload(completedUpload);

  TSUBA_PTP(tsuba::internal::FaultSensitivity::Normal);
  auto completeUploadOutcome =
      s3_client->CompleteMultipartUpload(completeMultipartUploadRequest);

  if (!completeUploadOutcome.IsSuccess()) {
    const auto& error = completeUploadOutcome.GetError();
    GALOIS_LOG_ERROR(
        "\n  Failed to complete multipart upload\n  {}: {}\n  [{}] {}",
        error.GetExceptionName(), error.GetMessage(), bucket, object);
    return ErrorCode::S3Error;
  }
  return galois::ResultSuccess();
}

internal::PutMultiHandle
internal::S3PutMultiAsync1(
    const std::string& bucket, const std::string& object, const uint8_t* data,
    uint64_t size) {
  GALOIS_LOG_VASSERT(
      library_init == true, "Must call tsuba::Init before S3 interaction");
  // We don't expect this function to be called directly, it is part of
  // s3_internal S3PutAsync checks the size and never calls S3PutMultiAsync1
  // unless the size is larger than kDefaultS3Region
  GALOIS_LOG_VASSERT(
      size > 0, "MultiAsync is a bad choice for a zero size file");

  Aws::S3::Model::CreateMultipartUploadRequest createMpRequest;
  createMpRequest.WithBucket(ToAwsString(bucket));
  createMpRequest.WithContentType("application/octet-stream");
  createMpRequest.WithKey(ToAwsString(object));

  SegmentedBufferView bufView = SegmentBuf(UINT64_C(0), data, size);

  auto pm = internal::PutMultiHandle{.impl_ = new internal::PutMultiImpl()};
  pm.impl_->parts_ =
      std::vector<SegmentedBufferView::BufPart>(bufView.begin(), bufView.end());
  pm.impl_->sema.SetGoal(std::distance(bufView.begin(), bufView.end()));
  pm.impl_->create_fut_ =
      async_s3_client->CreateMultipartUploadCallable(createMpRequest);
  pm.impl_->part_e_tags_.resize(bufView.NumSegments());
  pm.impl_->upload_id_ = "";

  GALOIS_LOG_DEBUG(
      "[{}]{:<30} PutMultiAsync1 size {:#x} nSeg {:d} parts_.size() {:d}",
      bucket, object, size, bufView.NumSegments(), pm.impl_->parts_.size());
  return pm;
}

galois::Result<void>
internal::S3PutMultiAsync2(
    const std::string& bucket, const std::string& object,
    internal::PutMultiHandle pmh) {
  // Standard says we can keep a pointer to value that remains valid even if
  // iterator is invalidated.  Iterators can be invalidated because of "rehash"
  // https://en.cppreference.com/w/cpp/container/unordered_map (Iterator
  // invalidation)

  auto createMpResponse = pmh.impl_->create_fut_.get();  // Blocking call

  if (!createMpResponse.IsSuccess()) {
    const auto& error = createMpResponse.GetError();
    GALOIS_LOG_ERROR(
        "Failed to create a multi-part upload request.\n  Bucket: "
        "[{}] Key: [{}]\n  {}: {}\n",
        bucket, object, error.GetExceptionName(), error.GetMessage());
    return ErrorCode::S3Error;
  }

  pmh.impl_->upload_id_ = createMpResponse.GetResult().GetUploadId();
  GALOIS_LOG_DEBUG(
      "[{}]{:<30} PutMultiAsync2 B parts.size() {:d}\n  upload id {}", bucket,
      object, pmh.impl_->parts_.size(), pmh.impl_->upload_id_);

  Aws::S3::Model::CompletedMultipartUpload completedUpload;
  for (unsigned i = 0; i < pmh.impl_->parts_.size(); ++i) {
    auto& part = pmh.impl_->parts_[i];
    auto lengthToWrite = part.end - part.start;
    auto streamBuf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>(
        kAwsTag, part.dest, static_cast<size_t>(lengthToWrite));
    auto preallocatedStreamReader =
        Aws::MakeShared<Aws::IOStream>(kAwsTag, streamBuf);
    Aws::S3::Model::UploadPartRequest uploadPartRequest;

    uploadPartRequest.WithBucket(ToAwsString(bucket))
        .WithContentLength(static_cast<long long>(lengthToWrite))
        .WithKey(ToAwsString(object))
        .WithPartNumber(i + 1) /* part numbers start at 1 */
        .WithUploadId(ToAwsString(pmh.impl_->upload_id_));

    uploadPartRequest.SetBody(preallocatedStreamReader);
    uploadPartRequest.SetContentType("application/octet-stream");

    // References to locals will go out of scope
    auto callback = [=](const Aws::S3::S3Client* /*client*/,
                        const Aws::S3::Model::UploadPartRequest& request,
                        const Aws::S3::Model::UploadPartOutcome& outcome,
                        const std::shared_ptr<
                            const Aws::Client::AsyncCallerContext>& /*ctx*/) {
      if (outcome.IsSuccess()) {
        // "Concurrently accessing or modifying different elements is safe."
        pmh.impl_->part_e_tags_[i] = outcome.GetResult().GetETag();
        pmh.impl_->sema.GoalMinusOne();
        GALOIS_LOG_DEBUG(
            "[{}]{:<30} PutMultiAsync2 i {:d}\n etag {}", bucket, object, i,
            outcome.GetResult().GetETag());
      } else {
        /* TODO there are likely some errors we can handle gracefully
         * i.e., with retries */
        const auto& error = outcome.GetError();
        GALOIS_LOG_FATAL(
            "\n  Upload failed: {}: {}\n  upload_id: {}\n  [{}] {}",
            error.GetExceptionName(), error.GetMessage(), request.GetUploadId(),
            bucket, object);
      }
    };
    async_s3_client->UploadPartAsync(uploadPartRequest, callback);
  }

  return galois::ResultSuccess();
}

galois::Result<void>
internal::S3PutMultiAsync3(
    const std::string& bucket, const std::string& object,
    internal::PutMultiHandle pmh) {
  // Possibly blocking call
  pmh.impl_->sema.WaitGoal();

  Aws::S3::Model::CompletedMultipartUpload completedUpload;
  for (unsigned i = 0; i < pmh.impl_->part_e_tags_.size(); ++i) {
    Aws::S3::Model::CompletedPart completedPart;
    completedPart.WithPartNumber(i + 1).WithETag(
        ToAwsString(pmh.impl_->part_e_tags_[i]));
    completedUpload.AddParts(completedPart);
  }

  Aws::S3::Model::CompleteMultipartUploadRequest completeMultipartUploadRequest;
  completeMultipartUploadRequest.WithBucket(ToAwsString(bucket))
      .WithKey(ToAwsString(object))
      .WithUploadId(ToAwsString(pmh.impl_->upload_id_))
      .WithMultipartUpload(completedUpload);

  pmh.impl_->outcome_fut_ = async_s3_client->CompleteMultipartUploadCallable(
      completeMultipartUploadRequest);

  return galois::ResultSuccess();
}

galois::Result<void>
internal::S3PutMultiAsyncFinish(
    const std::string& bucket, const std::string& object,
    internal::PutMultiHandle pmh) {
  auto completeUploadOutcome = pmh.impl_->outcome_fut_.get();  // Blocking call

  delete pmh.impl_;

  if (!completeUploadOutcome.IsSuccess()) {
    /* TODO there are likely some errors we can handle gracefully */
    const auto& error = completeUploadOutcome.GetError();
    GALOIS_LOG_ERROR(
        "\n  Failed to complete mutipart upload\n  {}: {}\n  "
        "[{}] {}",
        error.GetExceptionName(), error.GetMessage(), bucket, object);
    return ErrorCode::S3Error;
  }
  return galois::ResultSuccess();
}

galois::Result<void>
internal::S3PutSingleAsync(
    const std::string& bucket, const std::string& object, const uint8_t* data,
    uint64_t size, internal::CountingSemaphore* sema) {
  Aws::S3::Model::PutObjectRequest object_request;
  GALOIS_LOG_VASSERT(
      library_init == true, "Must call tsuba::Init before S3 interaction");

  sema->SetGoal(1);

  object_request.SetBucket(ToAwsString(bucket));
  object_request.SetKey(ToAwsString(object));
  auto streamBuf = Aws::New<Aws::Utils::Stream::PreallocatedStreamBuf>(
      kAwsTag, (uint8_t*)data, static_cast<size_t>(size));
  auto preallocatedStreamReader =
      Aws::MakeShared<Aws::IOStream>(kAwsTag, streamBuf);

  object_request.SetBody(preallocatedStreamReader);
  object_request.SetContentType("application/octet-stream");

  auto callback = [=](const Aws::S3::S3Client* /*client*/,
                      const Aws::S3::Model::PutObjectRequest& /*request*/,
                      const Aws::S3::Model::PutObjectOutcome& outcome,
                      const std::shared_ptr<
                          const Aws::Client::AsyncCallerContext>& /*ctx*/) {
    if (outcome.IsSuccess()) {
      sema->GoalMinusOne();
    } else {
      /* TODO there are likely some errors we can handle gracefully
       * i.e., with retries */
      const auto& error = outcome.GetError();
      GALOIS_LOG_FATAL(
          "\n  Failed to complete single async upload\n  {}: {}\n  [{}] {}",
          error.GetExceptionName(), error.GetMessage(), bucket, object);
    }
  };
  async_s3_client->PutObjectAsync(object_request, callback);

  return galois::ResultSuccess();
}

galois::Result<void>
internal::S3PutSingleAsyncFinish(internal::CountingSemaphore* sema) {
  sema->WaitGoal();
  return galois::ResultSuccess();
}

galois::Result<std::unique_ptr<FileAsyncWork>>
S3PutAsync(
    const std::string& bucket, const std::string& object, const uint8_t* data,
    uint64_t size) {
  if (size < kS3DefaultBufSize) {
    auto future = std::async([=]() -> galois::Result<void> {
      auto sema = internal::CountingSemaphore();
      if (auto res =
              internal::S3PutSingleAsync(bucket, object, data, size, &sema);
          !res) {
        return res.error();
      }
      if (auto res = internal::S3PutSingleAsyncFinish(&sema); !res) {
        return res.error();
      }
      return galois::ResultSuccess();
    });
    return std::make_unique<FileAsyncWork>(std::move(future));
  } else {
    auto future = std::async([=]() -> galois::Result<void> {
      auto pm = internal::S3PutMultiAsync1(bucket, object, data, size);
      if (auto res = internal::S3PutMultiAsync2(bucket, object, pm); !res) {
        return res.error();
      }
      if (auto res = internal::S3PutMultiAsync3(bucket, object, pm); !res) {
        return res.error();
      }
      if (auto res = internal::S3PutMultiAsyncFinish(bucket, object, pm);
          !res) {
        return res.error();
      }
      return galois::ResultSuccess();
    });
    return std::make_unique<FileAsyncWork>(std::move(future));
  }
}

static void
PrepareObjectRequest(
    Aws::S3::Model::GetObjectRequest* object_request, const std::string& bucket,
    const std::string& object, SegmentedBufferView::BufPart part) {
  object_request->SetBucket(ToAwsString(bucket));
  object_request->SetKey(ToAwsString(object));
  std::ostringstream range;
  /* Knock one byte off the end because range in AWS S3 API is inclusive */
  range << "bytes=" << part.start << "-" << part.end - 1;
  object_request->SetRange(ToAwsString(range.str()));

  object_request->SetResponseStreamFactory([part]() {
    auto* bufferStream = Aws::New<Aws::Utils::Stream::DefaultUnderlyingStream>(
        kAwsTag, Aws::MakeUnique<Aws::Utils::Stream::PreallocatedStreamBuf>(
                     kAwsTag, part.dest, part.end - part.start + 1));
    if (bufferStream == nullptr) {
      abort();
    }
    return bufferStream;
  });
}

galois::Result<void>
internal::S3GetMultiAsync(
    const std::string& bucket, const std::string& object, uint64_t start,
    uint64_t size, uint8_t* result_buf, internal::CountingSemaphore* sema) {
  SegmentedBufferView bufView = SegmentBuf(start, result_buf, size);
  std::vector<SegmentedBufferView::BufPart> parts(
      bufView.begin(), bufView.end());
  if (parts.empty()) {
    return galois::ResultSuccess();
  }

  sema->SetGoal(parts.size());

  auto callback = [=](const Aws::S3::S3Client* /*client*/,
                      const Aws::S3::Model::GetObjectRequest& /*request*/,
                      const Aws::S3::Model::GetObjectOutcome& outcome,
                      const std::shared_ptr<
                          const Aws::Client::AsyncCallerContext>& /*ctx*/) {
    if (outcome.IsSuccess()) {
      sema->GoalMinusOne();
    } else {
      /* TODO there are likely some errors we can handle gracefully
       * i.e., with retries */
      const auto& error = outcome.GetError();
      GALOIS_LOG_FATAL(
          "\n  Failed to complete multi async download\n  {}: {}\n  [{}] {}",
          error.GetExceptionName(), error.GetMessage(), bucket, object);
    }
  };

  for (auto& part : parts) {
    Aws::S3::Model::GetObjectRequest request;
    PrepareObjectRequest(&request, bucket, object, part);
    async_s3_client->GetObjectAsync(request, callback);
  }
  return galois::ResultSuccess();
}

galois::Result<void>
internal::S3GetMultiAsyncFinish(internal::CountingSemaphore* sema) {
  sema->WaitGoal();
  // result_buf should have the data here
  return galois::ResultSuccess();
}

galois::Result<std::unique_ptr<FileAsyncWork>>
S3GetAsync(
    const std::string& bucket, const std::string& object, uint64_t start,
    uint64_t size, uint8_t* result_buf) {
  if (size == (uint64_t)0) {
    return galois::ResultSuccess();
  }
  auto future = std::async([=]() -> galois::Result<void> {
    auto sema = internal::CountingSemaphore();
    if (auto res = internal::S3GetMultiAsync(
            bucket, object, start, size, result_buf, &sema);
        !res) {
      return res.error();
    }
    if (auto res = internal::S3GetMultiAsyncFinish(&sema); !res) {
      return res.error();
    }
    return galois::ResultSuccess();
  });
  return std::make_unique<FileAsyncWork>(std::move(future));
}

galois::Result<void>
S3DownloadRange(
    const std::string& bucket, const std::string& object, uint64_t start,
    uint64_t size, uint8_t* result_buf) {
  auto s3_client = GetS3Client();
  SegmentedBufferView bufView = SegmentBuf(start, result_buf, size);
  std::vector<SegmentedBufferView::BufPart> parts(
      bufView.begin(), bufView.end());
  if (parts.empty()) {
    return galois::ResultSuccess();
  }

  if (parts.size() == 1) {
    /* skip all of the thread management overhead if we only have one request */
    Aws::S3::Model::GetObjectRequest request;
    PrepareObjectRequest(&request, bucket, object, parts[0]);
    Aws::S3::Model::GetObjectOutcome outcome = s3_client->GetObject(request);
    if (auto res = CheckS3Error(outcome); !res) {
      if (res.error() != ErrorCode::S3Error) {
        // TODO there are likely some errors we can handle gracefully
        // i.e., with retries
        const auto& error = outcome.GetError();
        GALOIS_LOG_ERROR(
            "\n  Failed S3DownloadRange\n  {}: {}\n [{}] {}",
            error.GetExceptionName(), error.GetMessage(), bucket, object);
      }
      return res.error();
    }
    // result_buf should have the data here
    return galois::ResultSuccess();
  }

  std::mutex m;
  std::condition_variable cv;
  uint64_t finished = 0;
  auto callback =
      [&](const Aws::S3::S3Client* /*clnt*/,
          const Aws::S3::Model::GetObjectRequest& /*req*/,
          const Aws::S3::Model::GetObjectOutcome& get_object_outcome,
          const std::shared_ptr<
              const Aws::Client::AsyncCallerContext>& /*ctx*/) {
        if (get_object_outcome.IsSuccess()) {
          /* result_buf should have our data here */
          std::unique_lock<std::mutex> lk(m);
          finished++;
          cv.notify_one();
        } else {
          /* TODO there are likely some errors we can handle gracefully
           * i.e., with retries */
          const auto& error = get_object_outcome.GetError();
          GALOIS_LOG_FATAL(
              "\n  Failed S3DownloadRange callback\n  {}: {}\n  [{}] {}",
              error.GetExceptionName(), error.GetMessage(), bucket, object);
        }
      };
  for (auto& part : parts) {
    Aws::S3::Model::GetObjectRequest object_request;
    PrepareObjectRequest(&object_request, bucket, object, part);
    s3_client->GetObjectAsync(object_request, callback);
  }

  std::unique_lock<std::mutex> lk(m);
  cv.wait(lk, [&] { return finished >= parts.size(); });

  return galois::ResultSuccess();
}

// S3 only returns 1,000 directory entries at once, so to test this,
// use a big directory, e.g., this way
// tsuba_fault -c 2000 URI
// tsuba_gc -n -v  URI; tsuba_gc URI
galois::Result<std::unique_ptr<FileAsyncWork>>
S3ListAsync(
    const std::string& bucket, const std::string& object,
    std::unordered_set<std::string>* list) {
  GALOIS_LOG_VASSERT(
      library_init == true, "Must call tsuba::Init before S3 interaction");

  auto future = std::async([=]() -> galois::Result<void> {
    std::string token{};
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(ToAwsString(bucket));
    request.SetPrefix(ToAwsString(object));
    do {
      if (token.length() > 0) {
        request.SetContinuationToken(ToAwsString(token));
      }

      auto s3outcome = async_s3_client->ListObjectsV2(request);
      if (!s3outcome.IsSuccess()) {
        const auto& error = s3outcome.GetError();
        GALOIS_LOG_FATAL(
            "\n  Failed ListAsyncAW\n  {}: {}\n  [{}] {}",
            error.GetExceptionName(), error.GetMessage(), bucket, object);
      }
      auto s3result = s3outcome.GetResult();
      for (const auto& content : s3result.GetContents()) {
        std::string short_name(FromAwsString(content.GetKey()));
        size_t begin = short_name.find(object);
        GALOIS_LOG_ASSERT(begin == 0);
        GALOIS_LOG_ASSERT(short_name[object.length()] == '/');
        short_name.erase(begin, object.length() + 1);
        list->emplace(short_name);
      }
      token.clear();
      if (s3result.GetIsTruncated()) {
        // Get more listings
        token = FromAwsString(s3result.GetNextContinuationToken());
        GALOIS_LOG_VASSERT(
            token.length() > 0, "ListAsync IsTruncated true, but token empty");
      }
    } while (token.size() > 0);
    return galois::ResultSuccess();
  });
  return std::make_unique<FileAsyncWork>(std::move(future));
}

static galois::Result<void>
S3SendDelete(
    const Aws::Vector<Aws::S3::Model::ObjectIdentifier>& aws_objs,
    const std::string& bucket) {
  Aws::S3::Model::DeleteObjectsRequest object_request;
  Aws::S3::Model::Delete aws_delete;
  aws_delete.SetObjects(aws_objs);
  object_request.SetDelete(aws_delete);
  object_request.SetBucket(ToAwsString(bucket));

  GALOIS_LOG_DEBUG(
      "\n  DELETE [{}] files: {} {}\n", bucket, aws_objs.size(),
      (*aws_objs.begin()).GetKey());
  auto s3_client = GetS3Client();
  auto s3outcome = s3_client->DeleteObjects(object_request);
  if (!s3outcome.IsSuccess()) {
    GALOIS_LOG_DEBUG(
        "\n  Failed Delete\n  {}: {}\n  [{}]",
        s3outcome.GetError().GetExceptionName(),
        s3outcome.GetError().GetMessage(), bucket);
    return ErrorCode::S3Error;
  }
  return galois::ResultSuccess();
}

galois::Result<void>
S3Delete(
    const std::string& bucket, const std::string& object,
    const std::unordered_set<std::string>& files) {
  galois::Result<void> res = galois::ResultSuccess();
  if (files.size() == 0) {
    return res;
  }
  Aws::Vector<Aws::S3::Model::ObjectIdentifier> aws_objs;

  uint64_t index = 0;
  for (const auto& file : files) {
    // Must send a batch of kS3MaxDelete or fewer at a time
    if (index && (index % kS3MaxDelete) == 0) {
      auto batch_res = S3SendDelete(aws_objs, bucket);
      // If we get an error code, carry on, but remember it and return it
      if (!batch_res) {
        res = batch_res;
      }
      aws_objs.clear();
    }
    Aws::S3::Model::ObjectIdentifier oid;
    oid.SetKey(ToAwsString(galois::JoinPath(object, file)));
    aws_objs.push_back(oid);
    index++;
  }

  if (aws_objs.size() > 0) {
    auto batch_res = S3SendDelete(aws_objs, bucket);
    if (!batch_res) {
      res = batch_res;
    }
  }

  return res;
}

} /* namespace tsuba */
