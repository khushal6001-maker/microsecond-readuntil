// src/transport/proto_compat.hpp
//
// Compile-time guard on the MinKNOW wire schema.
//
// Why this file exists
// --------------------
// Protobuf compatibility is by FIELD NUMBER, not by field name, and a mismatch
// is silent: an unexpected number is skipped as an unknown field, so signal
// simply stops arriving and actions simply stop taking effect. Nothing crashes
// and nothing logs. On a sequencer that means a run that looks healthy while
// doing no adaptive sampling at all.
//
// So every field number this engine depends on is asserted here against the
// generated headers. Bump the submodule and the build breaks loudly, naming the
// field, instead of the daemon going quietly inert.
//
// Pinned to minknow_api 6.10.3 (tag), verified against
// third_party/minknow_api/proto/minknow_api/data.proto.
//
// The schema facts that drive the engine's design:
//   * ReadData.number was REMOVED (`reserved 2; // removed since 6.0`), so an
//     action can only identify a read by its id STRING. See read_state.hpp for
//     how that is kept off the hot path.
//   * GetLiveReadsResponse.channels is field 4, NOT 3. Easy to get wrong by
//     counting declarations instead of reading the numbers.
//   * Actions.actions is field 2, NOT 1.
#pragma once

#if !defined(MRU_WITH_PROTOBUF)
#error "proto_compat.hpp requires the generated minknow_api headers (-DMRU_WITH_TRANSPORT=ON)"
#endif

#include <type_traits>

#include "minknow_api/data.grpc.pb.h"
#include "minknow_api/data.pb.h"
#include "transport/chunk_ref.hpp"

namespace mru::proto_compat {

using Req = minknow_api::data::GetLiveReadsRequest;
using Resp = minknow_api::data::GetLiveReadsResponse;
using ReadData = Resp::ReadData;
using Action = Req::Action;
using StreamSetup = Req::StreamSetup;

// ---------------------------------------------------------------------------
// GetLiveReadsResponse
// ---------------------------------------------------------------------------
static_assert(Resp::kSamplesSinceStartFieldNumber == 1, "samples_since_start moved");
static_assert(Resp::kSecondsSinceStartFieldNumber == 2, "seconds_since_start moved");
static_assert(Resp::kChannelsFieldNumber == 4,
              "channels moved -- it is field 4, not 3; ingestion would go silent");
static_assert(Resp::kActionResponsesFieldNumber == 5, "action_responses moved");

// channels must stay a map keyed by channel number: the reader shards on the key.
static_assert(std::is_same_v<std::decay_t<decltype(std::declval<const Resp&>().channels())>,
                             google::protobuf::Map<google::protobuf::uint32, ReadData>>,
              "channels is no longer map<uint32, ReadData>");

// ---------------------------------------------------------------------------
// ReadData -- every field the ingestion path reads
// ---------------------------------------------------------------------------
static_assert(ReadData::kIdFieldNumber == 1, "ReadData.id moved");
static_assert(ReadData::kStartSampleFieldNumber == 3, "ReadData.start_sample moved");
static_assert(ReadData::kChunkStartSampleFieldNumber == 4,
              "ReadData.chunk_start_sample moved");
static_assert(ReadData::kChunkLengthFieldNumber == 5, "ReadData.chunk_length moved");
static_assert(ReadData::kChunkClassificationsFieldNumber == 6,
              "ReadData.chunk_classifications moved");
static_assert(ReadData::kRawDataFieldNumber == 7,
              "ReadData.raw_data moved -- this is the signal itself");
static_assert(ReadData::kMedianBeforeFieldNumber == 8, "ReadData.median_before moved");
static_assert(ReadData::kMedianFieldNumber == 9, "ReadData.median moved");

// Detects the resurrection of ReadData.number. If this ever fires, the submodule
// has been moved BACK to a pre-6.0 minknow_api, and read_state.hpp's whole
// rationale (string ids only) no longer matches the API being talked to.
template <class T, class = void>
struct has_number : std::false_type {};
template <class T>
struct has_number<T, std::void_t<decltype(std::declval<const T&>().number())>>
    : std::true_type {};

static_assert(!has_number<ReadData>::value,
              "ReadData::number() exists: the minknow_api submodule is pinned to a "
              "pre-6.0 release. Either re-pin to the tag matching your MinKNOW Core "
              "minor version, or revisit read_state.hpp -- on 5.x the cheap uint32 "
              "read number is available and the inline id buffer is unnecessary.");
static_assert(!has_number<Action>::value,
              "Action::number() exists: see the note on ReadData::number above");

// ---------------------------------------------------------------------------
// Action -- the unblock path
// ---------------------------------------------------------------------------
static_assert(Action::kActionIdFieldNumber == 1, "Action.action_id moved");
static_assert(Action::kChannelFieldNumber == 2, "Action.channel moved");
static_assert(Action::kIdFieldNumber == 3,
              "Action.read.id moved -- unblocks would silently stop working");
static_assert(Action::kUnblockFieldNumber == 5, "Action.unblock moved");
static_assert(Action::kStopFurtherDataFieldNumber == 6, "Action.stop_further_data moved");
static_assert(Req::Actions::kActionsFieldNumber == 2,
              "Actions.actions moved -- it is field 2, not 1");
static_assert(Req::UnblockAction::kDurationFieldNumber == 1, "UnblockAction.duration moved");

// ---------------------------------------------------------------------------
// StreamSetup
// ---------------------------------------------------------------------------
static_assert(StreamSetup::kFirstChannelFieldNumber == 1, "first_channel moved");
static_assert(StreamSetup::kLastChannelFieldNumber == 2, "last_channel moved");
static_assert(StreamSetup::kRawDataTypeFieldNumber == 3, "raw_data_type moved");
static_assert(StreamSetup::kSampleMinimumChunkSizeFieldNumber == 4,
              "sample_minimum_chunk_size moved");
static_assert(StreamSetup::kAcceptedFirstChunkClassificationsFieldNumber == 7,
              "accepted_first_chunk_classifications moved");

// ---------------------------------------------------------------------------
// Our own RawDataType mirror must match the generated enum exactly, since the
// decide layer uses ours to compute bytes-per-sample.
// ---------------------------------------------------------------------------
static_assert(static_cast<int>(RawDataType::kKeepLast) == Req::KEEP_LAST, "KEEP_LAST");
static_assert(static_cast<int>(RawDataType::kNone) == Req::NONE, "NONE");
static_assert(static_cast<int>(RawDataType::kCalibrated) == Req::CALIBRATED,
              "CALIBRATED: a mismatch here silently halves or doubles the sample count");
static_assert(static_cast<int>(RawDataType::kUncalibrated) == Req::UNCALIBRATED,
              "UNCALIBRATED: see the note on CALIBRATED");

// ---------------------------------------------------------------------------
// ActionResponse: MinKNOW tells us when an unblock arrived too late. This is a
// server-attested latency metric and far better evidence than anything we can
// infer client-side, so the values are pinned too.
// ---------------------------------------------------------------------------
static_assert(Resp::ActionResponse::SUCCESS == 0, "ActionResponse.SUCCESS");
static_assert(Resp::ActionResponse::FAILED_READ_FINISHED == 1,
              "FAILED_READ_FINISHED: the read ended before our action landed");
static_assert(Resp::ActionResponse::FAILED_READ_TOO_LONG == 2,
              "FAILED_READ_TOO_LONG: we were too slow to unblock");

// The RPC must stay bidirectionally streaming; a unary fallback would change the
// entire threading model.
static_assert(
    std::is_same_v<
        decltype(std::declval<minknow_api::data::DataService::Stub&>().get_live_reads(
            std::declval<grpc::ClientContext*>())),
        std::unique_ptr<grpc::ClientReaderWriter<Req, Resp>>>,
    "get_live_reads is no longer a bidirectional stream of (Request, Response)");

}  // namespace mru::proto_compat
