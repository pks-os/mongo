/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <list>
#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/utility/in_place_factory.hpp>  // IWYU pragma: keep

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_helpers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_metadata.h"
#include "mongo/db/timeseries/bucket_catalog/flat_bson.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/bucket_compression_failure.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/tracking/unordered_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::timeseries::bucket_catalog::internal {
namespace {
MONGO_FAIL_POINT_DEFINE(alwaysUseSameBucketCatalogStripe);
MONGO_FAIL_POINT_DEFINE(hangTimeSeriesBatchPrepareWaitingForConflictingOperation);

stdx::mutex _bucketIdGenLock;
PseudoRandom _bucketIdGenPRNG(SecureRandom().nextInt64());
AtomicWord<uint64_t> _bucketIdGenCounter{static_cast<uint64_t>(_bucketIdGenPRNG.nextInt64())};

std::string rolloverActionToString(RolloverAction action) {
    switch (action) {
        case RolloverAction::kNone:
            return "kNone";
        case RolloverAction::kArchive:
            return "kArchive";
        case RolloverAction::kHardClose:
            return "kHardClose";
        case RolloverAction::kSoftClose:
            return "kSoftClose";
    }
    MONGO_UNREACHABLE;
}
void assertNoOpenUnclearedBucketsForKey(Stripe& stripe,
                                        BucketStateRegistry& registry,
                                        const BucketKey& key) {
    auto it = stripe.openBucketsByKey.find(key);
    if (it != stripe.openBucketsByKey.end()) {
        auto& openSet = it->second;
        for (Bucket* bucket : openSet) {
            if (bucket->rolloverAction == RolloverAction::kNone) {
                if (auto state = materializeAndGetBucketState(registry, bucket);
                    state && !conflictsWithInsertions(state.value())) {
                    for (Bucket* b : openSet) {
                        LOGV2_INFO(8999000,
                                   "Dumping buckets for key",
                                   "key"_attr = key.metadata,
                                   "bucketId"_attr = b->bucketId.oid,
                                   "bucketState"_attr = bucketStateToString(
                                       materializeAndGetBucketState(registry, b).value()),
                                   "rolloverAction"_attr =
                                       rolloverActionToString(b->rolloverAction));
                    }
                    invariant(bucket->rolloverAction != RolloverAction::kNone || !state ||
                              conflictsWithInsertions(state.value()));
                }
            }
        }
    }
}

/**
 * Abandons the write batch and notifies any waiters that the bucket has been cleared.
 */
void abortWriteBatch(WriteBatch& batch, const Status& status) {
    if (batch.promise.getFuture().isReady()) {
        return;
    }

    batch.promise.setError(status);
}

/**
 * Returns a prepared write batch matching the specified 'key' if one exists, by searching the set
 * of open buckets associated with 'key'.
 */
std::shared_ptr<WriteBatch> findPreparedBatch(const Stripe& stripe,
                                              WithLock stripeLock,
                                              const BucketKey& key,
                                              boost::optional<OID> oid) {
    auto it = stripe.openBucketsByKey.find(key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return {};
    }

    auto& openSet = it->second;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->preparedBatch &&
            (!oid.has_value() || oid.value() == potentialBucket->bucketId.oid)) {
            return potentialBucket->preparedBatch;
        }
    }

    return {};
}

/**
 * Finds an outstanding disk access operation that could conflict with reopening a bucket for the
 * series defined by 'key'. If one exists, this function returns an associated awaitable object.
 */
boost::optional<InsertWaiter> checkForWait(const Stripe& stripe,
                                           WithLock stripeLock,
                                           const BucketKey& key,
                                           boost::optional<OID> oid) {
    if (auto batch = findPreparedBatch(stripe, stripeLock, key, oid)) {
        return InsertWaiter{batch};
    }

    if (auto it = stripe.outstandingReopeningRequests.find(key);
        it != stripe.outstandingReopeningRequests.end()) {
        auto& requests = it->second;
        invariant(!requests.empty());

        if (!oid.has_value()) {
            // We are trying to perform a query-based reopening. This conflicts with any reopening
            // for the key.
            return InsertWaiter{requests.front()};
        }

        // We are about to attempt an archive-based reopening. This conflicts with any query-based
        // reopening for the key, or another archive-based reopening for this bucket.
        for (auto&& request : requests) {
            if (!request->oid.has_value() ||
                (request->oid.has_value() && request->oid.value() == oid.value())) {
                return InsertWaiter{request};
            }
        }
    }

    return boost::none;
}

tracking::shared_ptr<ExecutionStats> getEmptyStats() {
    static const auto kEmptyStats{std::make_shared<ExecutionStats>()};
    return kEmptyStats;
}

void doRollover(BucketCatalog& catalog,
                Stripe& stripe,
                WithLock stripeLock,
                Bucket& bucket,
                ClosedBuckets& closedBuckets,
                RolloverAction action) {
    invariant(action != RolloverAction::kNone);
    if (allCommitted(bucket)) {
        // The bucket does not contain any measurements that are yet to be committed, so we can take
        // action now.
        ExecutionStatsController stats = getExecutionStats(catalog, bucket.bucketId.collectionUUID);
        if (action == RolloverAction::kArchive) {
            archiveBucket(catalog, stripe, stripeLock, bucket, stats, closedBuckets);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, bucket, stats);
        }
    } else {
        // We must keep the bucket around until all measurements are committed, just mark
        // the action we chose now so it we know what to do when the last batch finishes.
        bucket.rolloverAction = action;
    }
}
}  // namespace

StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketKey& key) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return key.hash % catalog.stripes.size();
}

StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketId& bucketId) {
    if (MONGO_unlikely(alwaysUseSameBucketCatalogStripe.shouldFail())) {
        return 0;
    }
    return bucketId.keySignature % catalog.stripes.size();
}

StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    tracking::Context& trackingContext,
    const UUID& collectionUUID,
    const TimeseriesOptions& options,
    const BSONObj& doc) {
    Date_t time;
    BSONElement metadata;

    if (!options.getMetaField().has_value()) {
        auto swTime = extractTime(doc, options.getTimeField());
        if (!swTime.isOK()) {
            return swTime.getStatus();
        }
        time = swTime.getValue();
    } else {
        auto swDocTimeAndMeta =
            extractTimeAndMeta(doc, options.getTimeField(), options.getMetaField().value());
        if (!swDocTimeAndMeta.isOK()) {
            return swDocTimeAndMeta.getStatus();
        }
        time = swDocTimeAndMeta.getValue().first;
        metadata = swDocTimeAndMeta.getValue().second;
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{collectionUUID,
                         BucketMetadata{trackingContext, metadata, options.getMetaField()}};

    return {std::make_pair(std::move(key), time)};
}


const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock,
                         const BucketId& bucketId,
                         IgnoreBucketState mode) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        if (mode == IgnoreBucketState::kYes) {
            return it->second.get();
        }

        if (auto state = materializeAndGetBucketState(registry, it->second.get());
            state && !conflictsWithInsertions(state.value())) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* useBucket(BucketStateRegistry& registry,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const BucketId& bucketId,
                  IgnoreBucketState mode) {
    return const_cast<Bucket*>(findBucket(registry, stripe, stripeLock, bucketId, mode));
}

Bucket* useBucketAndChangePreparedState(BucketStateRegistry& registry,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        const BucketId& bucketId,
                                        BucketPrepareAction prepare) {
    auto it = stripe.openBucketsById.find(bucketId);
    if (it != stripe.openBucketsById.end()) {
        StateChangeSuccessful stateChangeResult = (prepare == BucketPrepareAction::kPrepare)
            ? prepareBucketState(registry, it->second.get()->bucketId, it->second.get())
            : unprepareBucketState(registry, it->second.get()->bucketId, it->second.get());
        if (stateChangeResult == StateChangeSuccessful::kYes) {
            return it->second.get();
        }
    }
    return nullptr;
}

Bucket* useBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  InsertContext& info,
                  AllowBucketCreation mode,
                  const Date_t& time,
                  const StringDataComparator* comparator) {
    auto it = stripe.openBucketsByKey.find(info.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return mode == AllowBucketCreation::kYes
            ? &allocateBucket(catalog, stripe, stripeLock, info, time, comparator)
            : nullptr;
    }

    absl::InlinedVector<Bucket*, 4> bucketsToCleanUp;
    auto cleanupFn = [&]() {
        ExecutionStatsController statsStorage;
        for (Bucket* bucket : bucketsToCleanUp) {
            statsStorage = getExecutionStats(catalog, bucket->bucketId.collectionUUID);
            abort(catalog,
                  stripe,
                  stripeLock,
                  *bucket,
                  statsStorage,
                  /*batch=*/nullptr,
                  getTimeseriesBucketClearedError(bucket->bucketId.oid));
        }
    };
    ScopeGuard cleanup{cleanupFn};

    auto& openSet = it->second;
    for (Bucket* potentialBucket : openSet) {
        if (potentialBucket->rolloverAction == RolloverAction::kNone) {
            auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, potentialBucket);
            if (state && !conflictsWithInsertions(state.value())) {
                markBucketNotIdle(stripe, stripeLock, *potentialBucket);
                return potentialBucket;
            } else if (state) {
                bucketsToCleanUp.push_back(potentialBucket);
            } else {
                // If state is missing, it was already aborted and is just waiting to be cleaned up
                // after the prepared batch is resolved.
                invariant(potentialBucket->preparedBatch);
            }
        }
    }

    cleanup.dismiss();
    cleanupFn();

    return mode == AllowBucketCreation::kYes
        ? &allocateBucket(catalog, stripe, stripeLock, info, time, comparator)
        : nullptr;
}

Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           InsertContext& insertContext,
                           const Date_t& time) {
    auto it = stripe.openBucketsByKey.find(insertContext.key);
    if (it == stripe.openBucketsByKey.end()) {
        // No open bucket for this metadata.
        return nullptr;
    }

    absl::InlinedVector<Bucket*, 4> bucketsToCleanUp;
    ScopeGuard cleanup{[&]() {
        ExecutionStatsController statsStorage;
        for (Bucket* bucket : bucketsToCleanUp) {
            statsStorage = getExecutionStats(catalog, bucket->bucketId.collectionUUID);
            abort(catalog,
                  stripe,
                  stripeLock,
                  *bucket,
                  statsStorage,
                  /*batch=*/nullptr,
                  getTimeseriesBucketClearedError(bucket->bucketId.oid));
        }
    }};

    auto& openSet = it->second;
    // In order to potentially erase elements of the set while we iterate it (via _abort), we need
    // to advance the iterator before we call erase. This means we can't use the more
    // straightforward range iteration, and use the somewhat awkward pattern below.
    for (auto it = openSet.begin(); it != openSet.end();) {
        Bucket* potentialBucket = *it++;

        if (potentialBucket->rolloverAction == RolloverAction::kNone ||
            potentialBucket->rolloverAction == RolloverAction::kHardClose) {
            continue;
        }

        auto bucketTime = potentialBucket->minTime;
        if (time - bucketTime >= Seconds(*insertContext.options.getBucketMaxSpanSeconds()) ||
            time < bucketTime) {
            continue;
        }

        auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, potentialBucket);
        if (state && !conflictsWithInsertions(state.value())) {
            invariant(!potentialBucket->idleListEntry.has_value());
            return potentialBucket;
        }

        // Clean up the bucket if it has been cleared.
        if (state && (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value()))) {
            bucketsToCleanUp.push_back(potentialBucket);
        }
    }

    return nullptr;
}

StatusWith<tracking::unique_ptr<Bucket>> rehydrateBucket(BucketCatalog& catalog,
                                                         ExecutionStatsController& stats,
                                                         const UUID& collectionUUID,
                                                         const StringDataComparator* comparator,
                                                         const TimeseriesOptions& options,
                                                         const BucketToReopen& bucketToReopen,
                                                         const uint64_t catalogEra,
                                                         const BucketKey* expectedKey) {
    ScopeGuard updateStatsOnError([&stats] { stats.incNumBucketReopeningsFailed(); });

    const auto& [bucketDoc, validator] = bucketToReopen;
    if (catalogEra < getCurrentEra(catalog.bucketStateRegistry)) {
        return {ErrorCodes::WriteConflict, "Bucket is from an earlier era, may be outdated"};
    }

    BSONElement bucketIdElem = bucketDoc.getField(kBucketIdFieldName);
    if (bucketIdElem.eoo() || bucketIdElem.type() != BSONType::jstOID) {
        return {ErrorCodes::BadValue,
                str::stream() << kBucketIdFieldName << " is missing or not an ObjectId"};
    }

    // Validate the bucket document against the schema.
    auto result = validator(bucketDoc);
    if (result.first != Collection::SchemaValidationResult::kPass) {
        return result.second;
    }

    auto controlField = bucketDoc.getObjectField(kBucketControlFieldName);
    auto closedElem = controlField.getField(kBucketControlClosedFieldName);
    if (closedElem.booleanSafe()) {
        return {ErrorCodes::BadValue,
                "Bucket has been marked closed and is not eligible for reopening"};
    }

    BSONElement metadata;
    auto metaFieldName = options.getMetaField();
    if (metaFieldName) {
        metadata = bucketDoc.getField(kBucketMetaFieldName);
    }

    // Buckets are spread across independently-lockable stripes to improve parallelism. We map a
    // bucket to a stripe by hashing the BucketKey.
    auto key = BucketKey{collectionUUID,
                         BucketMetadata{getTrackingContext(catalog.trackingContexts,
                                                           TrackingScope::kOpenBucketsById),
                                        metadata,
                                        options.getMetaField()}};
    if (expectedKey && key != *expectedKey) {
        return {ErrorCodes::BadValue, "Bucket metadata does not match (hash collision)"};
    }

    auto minTime = controlField.getObjectField(kBucketControlMinFieldName)
                       .getField(options.getTimeField())
                       .Date();
    BucketId bucketId{key.collectionUUID, bucketIdElem.OID(), key.signature()};
    tracking::unique_ptr<Bucket> bucket = tracking::make_unique<Bucket>(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
        catalog.trackingContexts,
        bucketId,
        std::move(key),
        options.getTimeField(),
        minTime,
        catalog.bucketStateRegistry);

    const bool isCompressed = isCompressedBucket(bucketDoc);

    bucket->isReopened = true;

    // Initialize the remaining member variables from the bucket document.
    bucket->size = bucketDoc.objsize();

    auto swMinMax = generateMinMaxFromBucketDoc(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kSummaries),
        bucketDoc,
        comparator);
    if (!swMinMax.isOK()) {
        return swMinMax.getStatus();
    }
    bucket->minmax = std::move(swMinMax.getValue());

    auto swSchema = generateSchemaFromBucketDoc(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kSummaries),
        bucketDoc,
        comparator);
    if (!swSchema.isOK()) {
        return swSchema.getStatus();
    }
    bucket->schema = std::move(swSchema.getValue());

    // Populate the top-level data field names.
    uint32_t numMeasurements = 0;
    const BSONObj& dataObj = bucketDoc.getObjectField(kBucketDataFieldName);
    const BSONElement timeColumnElem = dataObj.getField(options.getTimeField());

    if (isCompressed && timeColumnElem.type() == BSONType::BinData) {
        BSONColumn storage{timeColumnElem};
        numMeasurements = storage.size();
    } else if (timeColumnElem.isABSONObj()) {
        numMeasurements = timeColumnElem.Obj().nFields();
    } else {
        return {ErrorCodes::BadValue,
                "Bucket data field is malformed (missing a valid time column)"};
    }

    bucket->bucketIsSortedByTime = controlField.getField(kBucketControlVersionFieldName).Number() ==
            kTimeseriesControlCompressedSortedVersion
        ? true
        : false;
    bucket->numMeasurements = numMeasurements;
    bucket->numCommittedMeasurements = numMeasurements;

    if (isCompressed) {
        // Initialize BSONColumnBuilders from the compressed bucket data fields.
        try {
            bucket->measurementMap.initBuilders(dataObj, bucket->numCommittedMeasurements);
        } catch (const DBException& ex) {
            LOGV2_WARNING(
                8830601,
                "Failed to decompress bucket for time-series insert upon reopening, will retry "
                "insert on a new bucket",
                "error"_attr = ex,
                "bucketId"_attr = bucket->bucketId.oid,
                "collectionUUID"_attr = bucket->bucketId.collectionUUID);

            LOGV2_WARNING_OPTIONS(8852900,
                                  logv2::LogTruncation::Disabled,
                                  "Failed to decompress bucket for time-series insert upon "
                                  "reopening, will retry insert on a new bucket",
                                  "bucket"_attr =
                                      base64::encode(bucketDoc.objdata(), bucketDoc.objsize()));

            invariant(!TestingProctor::instance().isEnabled());
            timeseries::bucket_catalog::freeze(catalog, bucketId);
            return Status(
                BucketCompressionFailure(collectionUUID, bucketId.oid, bucketId.keySignature),
                ex.reason());
        }
    }

    updateStatsOnError.dismiss();
    return {std::move(bucket)};
}

StatusWith<std::reference_wrapper<Bucket>> reopenBucket(BucketCatalog& catalog,
                                                        Stripe& stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController& stats,
                                                        const BucketKey& key,
                                                        tracking::unique_ptr<Bucket>&& bucket,
                                                        std::uint64_t targetEra,
                                                        ClosedBuckets& closedBuckets) {
    invariant(bucket.get());

    expireIdleBuckets(catalog, stripe, stripeLock, key.collectionUUID, stats, closedBuckets);

    auto status = initializeBucketState(
        catalog.bucketStateRegistry, bucket->bucketId, bucket.get(), targetEra);

    // Forward the WriteConflict if the bucket has been cleared or has a pending direct write.
    if (!status.isOK()) {
        return status;
    }

    // If this bucket was archived, we need to remove it from the set of archived buckets.
    auto archivedKey = std::make_tuple(key.collectionUUID, key.hash, bucket->minTime);
    if (auto it = stripe.archivedBuckets.find(archivedKey); it != stripe.archivedBuckets.end()) {
        stripe.archivedBuckets.erase(it);
        stats.decNumActiveBuckets();
    }

    // Pass ownership of the reopened bucket to the bucket catalog.
    auto [insertedIt, newlyInserted] =
        stripe.openBucketsById.try_emplace(bucket->bucketId, std::move(bucket));
    invariant(newlyInserted);
    Bucket* unownedBucket = insertedIt->second.get();

    // If we already have an open bucket for this key, we need to close it.
    if (auto it = stripe.openBucketsByKey.find(key); it != stripe.openBucketsByKey.end()) {
        auto& openSet = it->second;
        for (Bucket* existingBucket : openSet) {
            if (existingBucket->rolloverAction == RolloverAction::kNone) {
                auto state =
                    materializeAndGetBucketState(catalog.bucketStateRegistry, unownedBucket);
                if (state && !conflictsWithInsertions(state.value())) {
                    stats.incNumBucketsClosedDueToReopening();
                    if (allCommitted(*existingBucket)) {
                        closeOpenBucket(catalog, stripe, stripeLock, *existingBucket, stats);
                    } else {
                        existingBucket->rolloverAction = RolloverAction::kSoftClose;
                    }
                    // We should only have one open, uncleared bucket at a time.
                    break;
                }
            }
        }
    }

    // Now actually mark this bucket as open.
    if constexpr (kDebugBuild) {
        assertNoOpenUnclearedBucketsForKey(stripe, catalog.bucketStateRegistry, key);
    }
    stripe.openBucketsByKey[key].emplace(unownedBucket);
    stats.incNumBucketsReopened();

    stats.incNumActiveBuckets();

    return *unownedBucket;
}

StatusWith<std::reference_wrapper<Bucket>> reuseExistingBucket(BucketCatalog& catalog,
                                                               Stripe& stripe,
                                                               WithLock stripeLock,
                                                               ExecutionStatsController& stats,
                                                               const BucketKey& key,
                                                               Bucket& existingBucket,
                                                               std::uint64_t targetEra) {
    // If we have an existing bucket, passing the Bucket* will let us check if the bucket was
    // cleared as part of a set since the last time it was used. If we were to just check by OID, we
    // may miss if e.g. there was a move chunk operation.
    auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, &existingBucket);
    invariant(state);
    if (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value())) {
        abort(catalog,
              stripe,
              stripeLock,
              existingBucket,
              stats,
              nullptr,
              getTimeseriesBucketClearedError(existingBucket.bucketId.oid));
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    } else if (transientlyConflictsWithReopening(state.value())) {
        // Avoid reusing the bucket if it conflicts with reopening.
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    // It's possible to have two buckets with the same ID in different collections, so let's make
    // extra sure the existing bucket is the right one.
    if (existingBucket.bucketId.collectionUUID != key.collectionUUID) {
        return {ErrorCodes::BadValue, "Cannot re-use bucket: same ID but different namespace"};
    }

    // If the bucket was already open, wasn't cleared, the state didn't conflict with reopening, and
    // the namespace matches, then we can simply return it.
    stats.incNumDuplicateBucketsReopened();
    markBucketNotIdle(stripe, stripeLock, existingBucket);

    return existingBucket;
}

std::variant<std::shared_ptr<WriteBatch>, RolloverReason> insertIntoBucket(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const BSONObj& doc,
    OperationId opId,
    AllowBucketCreation mode,
    InsertContext& insertContext,
    Bucket& existingBucket,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator,
    Bucket* excludedBucket,
    boost::optional<RolloverAction> excludedAction) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    Sizes sizesToBeAdded;

    bool isNewlyOpenedBucket = (existingBucket.size == 0);
    std::reference_wrapper<Bucket> bucketToUse{existingBucket};
    bool openedDueToMetadata = true;
    calculateBucketFieldsAndSizeChange(catalog.trackingContexts,
                                       bucketToUse.get(),
                                       doc,
                                       insertContext.options.getMetaField(),
                                       newFieldNamesToBeInserted,
                                       sizesToBeAdded);
    if (!isNewlyOpenedBucket) {
        auto [action, reason] =
            determineRolloverAction(catalog.trackingContexts,
                                    doc,
                                    insertContext,
                                    existingBucket,
                                    catalog.globalExecutionStats.numActiveBuckets.loadRelaxed(),
                                    newFieldNamesToBeInserted,
                                    sizesToBeAdded,
                                    mode,
                                    time,
                                    storageCacheSizeBytes,
                                    comparator);
        if ((action == RolloverAction::kSoftClose || action == RolloverAction::kArchive) &&
            mode == AllowBucketCreation::kNo) {
            // We don't actually want to roll this bucket over yet, bail out.
            return reason;
        } else if (action != RolloverAction::kNone) {
            openedDueToMetadata = false;
            bucketToUse = rollover(catalog,
                                   stripe,
                                   stripeLock,
                                   existingBucket,
                                   insertContext,
                                   action,
                                   time,
                                   comparator,
                                   excludedBucket,
                                   excludedAction);
            isNewlyOpenedBucket = true;
            // Recalculate the fields and size change for the newly opened bucket we got from
            // rolling over our original one.
            calculateBucketFieldsAndSizeChange(catalog.trackingContexts,
                                               bucketToUse.get(),
                                               doc,
                                               insertContext.options.getMetaField(),
                                               newFieldNamesToBeInserted,
                                               sizesToBeAdded);
        }
    }
    return addMeasurementToBatchAndBucket(catalog,
                                          doc,
                                          opId,
                                          insertContext,
                                          bucketToUse.get(),
                                          comparator,
                                          newFieldNamesToBeInserted,
                                          sizesToBeAdded,
                                          isNewlyOpenedBucket,
                                          openedDueToMetadata);
}

std::variant<std::shared_ptr<WriteBatch>, RolloverReason> tryToInsertIntoBucketWithoutRollover(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const BSONObj& doc,
    OperationId opId,
    AllowBucketCreation mode,
    InsertContext& insertContext,
    Bucket& bucketToInsertInto,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator) {
    Bucket::NewFieldNames newFieldNamesToBeInserted;
    Sizes sizesToBeAdded;

    bool isNewlyOpenedBucket = (bucketToInsertInto.size == 0);
    bool openedDueToMetadata = true;
    calculateBucketFieldsAndSizeChange(catalog.trackingContexts,
                                       bucketToInsertInto,
                                       doc,
                                       insertContext.options.getMetaField(),
                                       newFieldNamesToBeInserted,
                                       sizesToBeAdded);

    if (!isNewlyOpenedBucket) {
        auto [action, reason] =
            determineRolloverAction(catalog.trackingContexts,
                                    doc,
                                    insertContext,
                                    bucketToInsertInto,
                                    catalog.globalExecutionStats.numActiveBuckets.loadRelaxed(),
                                    newFieldNamesToBeInserted,
                                    sizesToBeAdded,
                                    mode,
                                    time,
                                    storageCacheSizeBytes,
                                    comparator);
        if (action != RolloverAction::kNone) {
            // We would not be able to insert this measurement without rolling over the bucket.
            // Return the reason for rolling over.
            return reason;
        }
    }

    return addMeasurementToBatchAndBucket(catalog,
                                          doc,
                                          opId,
                                          insertContext,
                                          bucketToInsertInto,
                                          comparator,
                                          newFieldNamesToBeInserted,
                                          sizesToBeAdded,
                                          isNewlyOpenedBucket,
                                          openedDueToMetadata);
}

void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch) {
    while (true) {
        boost::optional<InsertWaiter> waiter;
        {
            stdx::lock_guard stripeLock{stripe.mutex};
            Bucket* bucket =
                useBucket(registry, stripe, stripeLock, batch->bucketId, IgnoreBucketState::kNo);
            if (!bucket || isWriteBatchFinished(*batch)) {
                return;
            }

            if (bucket->preparedBatch) {
                // Conflict with other prepared batch on this bucket.
                waiter = bucket->preparedBatch;
            } else if (auto it = stripe.outstandingReopeningRequests.find(batch->bucketKey);
                       it != stripe.outstandingReopeningRequests.end()) {
                // Conflict with any query-based reopening request on this series (metaField value)
                // or an archive-based request on this bucket.
                auto& list = it->second;
                for (auto&& request : list) {
                    if (!request->oid.has_value() || request->oid.value() == batch->bucketId.oid) {
                        waiter = request;
                        break;
                    }
                }
            }

            if (!waiter.has_value()) {
                // No other batches for this bucket are currently committing, so we can proceed.
                bucket->preparedBatch = batch;
                bucket->batches.erase(batch->opId);
                return;
            }
        }
        invariant(waiter.has_value());

        // We only hit this failpoint when there are conflicting operations on the same series.
        hangTimeSeriesBatchPrepareWaitingForConflictingOperation.pauseWhileSet();

        // We have to wait for someone else to finish.
        waitToInsert(&waiter.value());  // We don't care about the result.
        waiter = boost::none;
    }
}

void removeBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  Bucket& bucket,
                  ExecutionStatsController& stats,
                  RemovalMode mode) {
    invariant(bucket.batches.empty());
    invariant(!bucket.preparedBatch);

    auto allIt = stripe.openBucketsById.find(bucket.bucketId);
    invariant(allIt != stripe.openBucketsById.end());

    markBucketNotIdle(stripe, stripeLock, bucket);

    // If the bucket was rolled over, then there may be a different open bucket for this metadata.
    auto openIt =
        stripe.openBucketsByKey.find({bucket.bucketId.collectionUUID, bucket.key.metadata});
    if (openIt != stripe.openBucketsByKey.end()) {
        auto& openSet = openIt->second;
        auto bucketIt = openSet.find(&bucket);
        if (bucketIt != openSet.end()) {
            if (openSet.size() == 1) {
                stripe.openBucketsByKey.erase(openIt);
            } else {
                openSet.erase(bucketIt);
            }
        }
    }

    // If we are cleaning up while archiving a bucket, then we want to preserve its state. Otherwise
    // we can remove the state from the catalog altogether.
    switch (mode) {
        case RemovalMode::kClose: {
            auto state = getBucketState(catalog.bucketStateRegistry, bucket.bucketId);
            // When removing a closed bucket, the BucketStateRegistry may contain state for this
            // bucket due to an untracked ongoing direct write (such as TTL delete).
            if (state.has_value()) {
                invariant(holds_alternative<DirectWriteCounter>(state.value()),
                          bucketStateToString(*state));
                invariant(get<DirectWriteCounter>(state.value()) < 0, bucketStateToString(*state));
            }
            break;
        }
        case RemovalMode::kAbort:
        case RemovalMode::kArchive:
            stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);
            break;
    }

    stats.decNumActiveBuckets();
    stripe.openBucketsById.erase(allIt);
}

void archiveBucket(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ExecutionStatsController& stats,
                   ClosedBuckets& closedBuckets) {
    bool archived =
        stripe.archivedBuckets
            .emplace(
                std::make_tuple(bucket.bucketId.collectionUUID, bucket.key.hash, bucket.minTime),
                ArchivedBucket{bucket.bucketId.oid})
            .second;

    if (archived) {
        // If we have an archived bucket, we still want to account for it in numberOfActiveBuckets
        // so we will increase it here since removeBucket decrements the count.
        stats.incNumActiveBuckets();
        removeBucket(catalog, stripe, stripeLock, bucket, stats, RemovalMode::kArchive);
    } else {
        // We had a meta hash collision, and already have a bucket archived with the same meta hash
        // and timestamp as this bucket. Since it's somewhat arbitrary which bucket we keep, we'll
        // keep the one that's already archived and just plain close this one.
        closeOpenBucket(catalog, stripe, stripeLock, bucket, stats);
    }
}

boost::optional<OID> findArchivedCandidate(
    BucketCatalog& catalog, Stripe& stripe, WithLock stripeLock, InsertContext& info, Date_t time) {

    // We want to find the largest time that is not greater than info.time. Generally
    // lower_bound will return the smallest element not less than the search value, but we are
    // using std::greater instead of std::less for the map's comparisons. This means the order
    // of keys will be reversed, and lower_bound will return what we want.
    auto it = stripe.archivedBuckets.lower_bound(
        std::make_tuple(info.key.collectionUUID, info.key.hash, time));
    if (it == stripe.archivedBuckets.end()) {
        return boost::none;
    }

    // Ensure we have an exact match on UUID and BucketKey::Hash
    const auto& uuid = std::get<UUID>(it->first);
    const auto& hash = std::get<BucketKey::Hash>(it->first);
    if (uuid != info.key.collectionUUID || hash != info.key.hash) {
        return boost::none;
    }

    const auto& candidateTime = std::get<Date_t>(it->first);
    invariant(candidateTime <= time);
    // We need to make sure our measurement can fit without violating max span. If not, we
    // can't use this bucket.
    if (time - candidateTime >= Seconds(*info.options.getBucketMaxSpanSeconds())) {
        return boost::none;
    }

    return it->second.oid;
}

std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(uint64_t storageCacheSizeBytes,
                                                         uint32_t workloadCardinality) {
    if (workloadCardinality == 0) {
        return {gTimeseriesBucketMaxSize, INT_MAX};
    }

    uint64_t intMax = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    int32_t derivedMaxSize = std::max(
        static_cast<int32_t>(std::min(storageCacheSizeBytes / (2 * workloadCardinality), intMax)),
        gTimeseriesBucketMinSize.load());
    return {std::min(gTimeseriesBucketMaxSize, derivedMaxSize), derivedMaxSize};
}

InsertResult getReopeningContext(BucketCatalog& catalog,
                                 Stripe& stripe,
                                 WithLock stripeLock,
                                 InsertContext& info,
                                 uint64_t catalogEra,
                                 AllowQueryBasedReopening allowQueryBasedReopening,
                                 const Date_t& time,
                                 uint64_t storageCacheSizeBytes) {
    if (auto archived = findArchivedCandidate(catalog, stripe, stripeLock, info, time)) {
        // Synchronize concurrent disk accesses. An outstanding query-based reopening request for
        // this series or an outstanding archived-based reopening request or prepared batch for this
        // bucket would potentially conflict with our choice to reopen here, so we must wait for any
        // such operation to finish and retry.
        if (auto waiter = checkForWait(stripe, stripeLock, info.key, archived.value())) {
            return waiter.value();
        }

        return ReopeningContext{
            catalog, stripe, stripeLock, info.key, catalogEra, archived.value()};
    }

    if (allowQueryBasedReopening == AllowQueryBasedReopening::kDisallow) {
        return ReopeningContext{catalog, stripe, stripeLock, info.key, catalogEra, {}};
    }

    // Synchronize concurrent disk accesses. An outstanding reopening request or prepared batch for
    // this series would potentially conflict with our choice to reopen here, so we must wait for
    // any such operation to finish and retry.
    if (auto waiter = checkForWait(stripe, stripeLock, info.key, boost::none)) {
        return waiter.value();
    }

    boost::optional<BSONElement> metaElement;
    if (info.options.getMetaField().has_value()) {
        metaElement = info.key.metadata.element();
    }

    auto controlMinTimePath = kControlMinFieldNamePrefix.toString() + info.options.getTimeField();
    auto maxDataTimeFieldPath = kDataFieldNamePrefix.toString() + info.options.getTimeField() +
        "." + std::to_string(gTimeseriesBucketMaxCount - 1);

    // Derive the maximum bucket size.
    auto [bucketMaxSize, _] = getCacheDerivedBucketMaxSize(
        storageCacheSizeBytes, catalog.globalExecutionStats.numActiveBuckets.loadRelaxed());

    return ReopeningContext{catalog,
                            stripe,
                            stripeLock,
                            info.key,
                            catalogEra,
                            generateReopeningPipeline(time,
                                                      metaElement,
                                                      controlMinTimePath,
                                                      maxDataTimeFieldPath,
                                                      *info.options.getBucketMaxSpanSeconds(),
                                                      bucketMaxSize)};
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Before we access the bucket, make sure it's still there.
    Bucket* bucket = useBucket(
        catalog.bucketStateRegistry, stripe, stripeLock, batch->bucketId, IgnoreBucketState::kYes);
    if (!bucket) {
        // Special case, bucket has already been cleared, and we need only abort this batch.
        abortWriteBatch(*batch, status);
        return;
    }

    // Proceed to abort any unprepared batches and remove the bucket if possible
    abort(catalog, stripe, stripeLock, *bucket, batch->stats, batch, status);
}

void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           ExecutionStatsController& stats,
           std::shared_ptr<WriteBatch> batch,
           const Status& status) {
    // Abort any unprepared batches. This should be safe since we have a lock on the stripe,
    // preventing anyone else from using these.
    for (const auto& [_, current] : bucket.batches) {
        abortWriteBatch(*current, status);
    }
    bucket.batches.clear();

    bool doRemove = true;  // We shouldn't remove the bucket if there's a prepared batch outstanding
                           // and it's not the one we manage. In that case, we don't know what the
                           // user is doing with it, but we need to keep the bucket around until
                           // that batch is finished.
    if (auto& prepared = bucket.preparedBatch) {
        if (batch && prepared == batch) {
            // We own the prepared batch, so we can go ahead and abort it and remove the bucket.
            abortWriteBatch(*prepared, status);
            prepared.reset();
        } else {
            doRemove = false;
        }
    }

    if (doRemove) {
        removeBucket(catalog, stripe, stripeLock, bucket, stats, RemovalMode::kAbort);
    } else {
        clearBucketState(catalog.bucketStateRegistry, bucket.bucketId);
    }
}

void markBucketIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    invariant(!bucket.idleListEntry.has_value());
    invariant(allCommitted(bucket));
    stripe.idleBuckets.push_front(&bucket);
    bucket.idleListEntry = stripe.idleBuckets.begin();
}

void markBucketNotIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket) {
    if (bucket.idleListEntry.has_value()) {
        stripe.idleBuckets.erase(bucket.idleListEntry.value());
        bucket.idleListEntry = boost::none;
    }
}

void expireIdleBuckets(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const UUID& collectionUUID,
                       ExecutionStatsController& collectionStats,
                       ClosedBuckets& closedBuckets) {
    // As long as we still need space and have entries and remaining attempts, close idle buckets.
    int32_t numExpired = 0;

    ExecutionStatsController storage;
    auto statsForBucket = [&](const BucketId& bucketId) -> ExecutionStatsController& {
        if (bucketId.collectionUUID == collectionUUID)
            return collectionStats;

        storage = getExecutionStats(catalog, bucketId.collectionUUID);
        return storage;
    };

    while (!stripe.idleBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {
        Bucket* bucket = stripe.idleBuckets.back();
        ExecutionStatsController& stats = statsForBucket(bucket->bucketId);

        auto state = materializeAndGetBucketState(catalog.bucketStateRegistry, bucket);
        if (state && !conflictsWithInsertions(state.value())) {
            // Can archive a bucket if it's still eligible for insertions.
            archiveBucket(catalog, stripe, stripeLock, *bucket, stats, closedBuckets);
            stats.incNumBucketsArchivedDueToMemoryThreshold();
        } else if (state &&
                   (isBucketStateCleared(state.value()) || isBucketStateFrozen(state.value()))) {
            // Bucket was cleared and just needs to be removed from catalog.
            removeBucket(catalog, stripe, stripeLock, *bucket, stats, RemovalMode::kAbort);
        } else {
            closeOpenBucket(catalog, stripe, stripeLock, *bucket, stats);
            stats.incNumBucketsClosedDueToMemoryThreshold();
        }

        ++numExpired;
    }

    while (!stripe.archivedBuckets.empty() &&
           getMemoryUsage(catalog) > catalog.memoryUsageThreshold() &&
           numExpired <= gTimeseriesIdleBucketExpiryMaxCountPerAttempt) {

        auto it = stripe.archivedBuckets.begin();
        const auto& [key, archived] = *it;
        const auto& uuid = std::get<UUID>(key);
        const auto& hash = std::get<BucketKey::Hash>(key);

        BucketId bucketId(uuid, archived.oid, BucketKey::signature(hash));
        ExecutionStatsController& stats = statsForBucket(bucketId);

        closeArchivedBucket(catalog, bucketId);

        stripe.archivedBuckets.erase(it);

        stats.decNumActiveBuckets();
        stats.incNumBucketsClosedDueToMemoryThreshold();
        ++numExpired;
    }
}

std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options) {
    OID oid;

    // We round the measurement timestamp down to the nearest minute, hour, or day depending on the
    // granularity. We do this for two reasons. The first is so that if measurements come in
    // slightly out of order, we don't have to close the current bucket due to going backwards in
    // time. The second, and more important reason, is so that we reliably group measurements
    // together into predictable chunks for sharding. This way we know from a measurement timestamp
    // what the bucket timestamp will be, so we can route measurements to the right shard chunk.
    auto roundedTime = roundTimestampToGranularity(time, options);
    int64_t const roundedSeconds = durationCount<Seconds>(roundedTime.toDurationSinceEpoch());
    oid.setTimestamp(roundedSeconds);

    // Now, if we used the standard OID generation method for the remaining bytes we could end up
    // with lots of bucket OID collisions. Consider the case where we have the granularity set to
    // 'Hours'. This means we will round down to the nearest day, so any bucket generated on the
    // same machine on the same day will have the same timestamp portion and unique instance portion
    // of the OID. Only the increment would differ. Since we only use 3 bytes for the increment
    // portion, we run a serious risk of overflow if we are generating lots of buckets.
    //
    // To address this, we'll instead use a PRNG to generate the rest of the bytes. With 8 bytes of
    // randomness, we should have a pretty low chance of collisions. The limit of the birthday
    // paradox converges to roughly the square root of the size of the space, so we would need a few
    // billion buckets with the same timestamp to expect collisions. In the rare case that we do get
    // a collision, we can (and do) simply regenerate the bucket _id at a higher level.
    uint64_t bits = BigEndian<uint64_t>::store(_bucketIdGenCounter.addAndFetch(1));

    OID::InstanceUnique instance;
    const auto instanceBuf = static_cast<uint8_t*>(instance.bytes);
    std::memcpy(instanceBuf, &bits, OID::kInstanceUniqueSize);

    OID::Increment increment;
    const auto incrementBuf = static_cast<uint8_t*>(increment.bytes);
    uint8_t* bitsBuf = (uint8_t*)&bits;
    std::memcpy(incrementBuf, &(bitsBuf)[OID::kInstanceUniqueSize], OID::kIncrementSize);

    oid.setInstanceUnique(instance);
    oid.setIncrement(increment);

    return {oid, roundedTime};
}

void resetBucketOIDCounter() {
    stdx::lock_guard lk{_bucketIdGenLock};
    _bucketIdGenCounter.store(static_cast<uint64_t>(_bucketIdGenPRNG.nextInt64()));
}

Bucket& allocateBucket(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       InsertContext& info,
                       const Date_t& time,
                       const StringDataComparator* comparator) {
    expireIdleBuckets(
        catalog, stripe, stripeLock, info.key.collectionUUID, info.stats, info.closedBuckets);

    // In rare cases duplicate bucket _id fields can be generated in the same stripe and fail to be
    // inserted. We will perform a limited number of retries to minimize the probability of
    // collision.
    auto maxRetries = gTimeseriesInsertMaxRetriesOnDuplicates.load();
    OID oid;
    Date_t roundedTime;
    tracking::unordered_map<BucketId, tracking::unique_ptr<Bucket>, BucketHasher>::iterator it;
    bool successfullyCreatedId = false;
    for (int retryAttempts = 0; !successfullyCreatedId && retryAttempts < maxRetries;
         ++retryAttempts) {
        std::tie(oid, roundedTime) = generateBucketOID(time, info.options);
        auto bucketId = BucketId{info.key.collectionUUID, oid, info.key.signature()};
        std::tie(it, successfullyCreatedId) = stripe.openBucketsById.try_emplace(
            bucketId,
            tracking::make_unique<Bucket>(
                getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
                catalog.trackingContexts,
                bucketId,
                info.key,
                info.options.getTimeField(),
                roundedTime,
                catalog.bucketStateRegistry));
        if (successfullyCreatedId) {
            Bucket* bucket = it->second.get();
            auto status = initializeBucketState(catalog.bucketStateRegistry, bucket->bucketId);
            if (!status.isOK()) {
                successfullyCreatedId = false;
            }
        }
        if (!successfullyCreatedId) {
            stripe.openBucketsById.erase(bucketId);
            resetBucketOIDCounter();
        }
    }
    uassert(6130900,
            "Unable to insert documents due to internal OID generation collision. Increase the "
            "value of server parameter 'timeseriesInsertMaxRetriesOnDuplicates' and try again",
            successfullyCreatedId);

    Bucket* bucket = it->second.get();
    if constexpr (kDebugBuild) {
        assertNoOpenUnclearedBucketsForKey(stripe, catalog.bucketStateRegistry, info.key);
    }
    stripe.openBucketsByKey[info.key].emplace(bucket);

    info.stats.incNumActiveBuckets();
    // Make sure we set the control.min time field to match the rounded _id timestamp.
    auto controlDoc = buildControlMinTimestampDoc(info.options.getTimeField(), roundedTime);
    bucket->minmax.update(controlDoc, /*metaField=*/boost::none, comparator);
    return *bucket;
}

Bucket& rollover(BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 InsertContext& info,
                 RolloverAction action,
                 const Date_t& time,
                 const StringDataComparator* comparator,
                 Bucket* additionalBucket,
                 boost::optional<RolloverAction> additionalAction) {
    doRollover(catalog, stripe, stripeLock, bucket, info.closedBuckets, action);
    if (additionalBucket) {
        invariant(additionalAction.has_value());
        doRollover(catalog,
                   stripe,
                   stripeLock,
                   *additionalBucket,
                   info.closedBuckets,
                   additionalAction.value());
    }

    return allocateBucket(catalog, stripe, stripeLock, info, time, comparator);
}

std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    TrackingContexts& trackingContexts,
    const BSONObj& doc,
    InsertContext& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    Sizes& sizesToBeAdded,
    AllowBucketCreation mode,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator) {
    // If the mode is enabled to create new buckets, then we should update stats for soft closures
    // accordingly. If we specify the mode to not allow bucket creation, it means we are not sure if
    // we want to soft close the bucket yet and should wait to update closure stats.
    const bool shouldUpdateStats = (mode == AllowBucketCreation::kYes);

    auto bucketTime = bucket.minTime;
    if (time - bucketTime >= Seconds(*info.options.getBucketMaxSpanSeconds())) {
        if (shouldUpdateStats) {
            info.stats.incNumBucketsClosedDueToTimeForward();
        }
        return {RolloverAction::kSoftClose, RolloverReason::kTimeForward};
    }
    if (time < bucketTime) {
        if (shouldUpdateStats) {
            info.stats.incNumBucketsArchivedDueToTimeBackward();
        }
        return {RolloverAction::kArchive, RolloverReason::kTimeBackward};
    }
    if (bucket.numMeasurements == static_cast<std::uint64_t>(gTimeseriesBucketMaxCount)) {
        info.stats.incNumBucketsClosedDueToCount();
        return {RolloverAction::kHardClose, RolloverReason::kCount};
    }

    // In scenarios where we have a high cardinality workload and face increased cache pressure we
    // will decrease the size of buckets before we close them.
    auto [effectiveMaxSize, cacheDerivedBucketMaxSize] =
        getCacheDerivedBucketMaxSize(storageCacheSizeBytes, numberOfActiveBuckets);

    // We restrict the ceiling of the bucket max size under cache pressure.
    int32_t absoluteMaxSize =
        std::min(Bucket::kLargeMeasurementsMaxBucketSize, cacheDerivedBucketMaxSize);
    if (bucket.size + sizesToBeAdded.total() > effectiveMaxSize) {
        bool keepBucketOpenForLargeMeasurements =
            bucket.numMeasurements < static_cast<std::uint64_t>(gTimeseriesBucketMinCount);
        if (keepBucketOpenForLargeMeasurements) {
            if (bucket.size + sizesToBeAdded.total() > absoluteMaxSize) {
                if (absoluteMaxSize != Bucket::kLargeMeasurementsMaxBucketSize) {
                    info.stats.incNumBucketsClosedDueToCachePressure();
                    return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
                }
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }

            // There's enough space to add this measurement and we're still below the large
            // measurement threshold.
            if (!bucket.keptOpenDueToLargeMeasurements) {
                // Only increment this metric once per bucket.
                bucket.keptOpenDueToLargeMeasurements = true;
                info.stats.incNumBucketsKeptOpenDueToLargeMeasurements();
            }
            return {RolloverAction::kNone, RolloverReason::kNone};
        } else {
            if (effectiveMaxSize == gTimeseriesBucketMaxSize) {
                info.stats.incNumBucketsClosedDueToSize();
                return {RolloverAction::kHardClose, RolloverReason::kSize};
            }
            info.stats.incNumBucketsClosedDueToCachePressure();
            return {RolloverAction::kHardClose, RolloverReason::kCachePressure};
        }
    }

    if (schemaIncompatible(bucket, doc, info.options.getMetaField(), comparator)) {
        info.stats.incNumBucketsClosedDueToSchemaChange();
        return {RolloverAction::kHardClose, RolloverReason::kSchemaChange};
    }

    return {RolloverAction::kNone, RolloverReason::kNone};
}

ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};
    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    auto res =
        catalog.executionStats.emplace(collectionUUID,
                                       tracking::make_shared<ExecutionStats>(getTrackingContext(
                                           catalog.trackingContexts, TrackingScope::kStats)));
    return {res.first->second, catalog.globalExecutionStats};
}

ExecutionStatsController getExecutionStats(BucketCatalog& catalog, const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return {it->second, catalog.globalExecutionStats};
    }

    // If the collection doesn't exist, return a set we can send stats into the void for.
    auto emptyStats = getEmptyStats();
    return {emptyStats, *emptyStats};
}

tracking::shared_ptr<ExecutionStats> getCollectionExecutionStats(const BucketCatalog& catalog,
                                                                 const UUID& collectionUUID) {
    stdx::lock_guard catalogLock{catalog.mutex};

    auto it = catalog.executionStats.find(collectionUUID);
    if (it != catalog.executionStats.end()) {
        return it->second;
    }
    return nullptr;
}

std::pair<UUID, tracking::shared_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog) {
    stdx::lock_guard catalogLock{sideBucketCatalog.mutex};
    invariant(sideBucketCatalog.executionStats.size() == 1);
    return *sideBucketCatalog.executionStats.begin();
}

void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        tracking::shared_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID) {
    ExecutionStatsController stats = getOrInitializeExecutionStats(catalog, collectionUUID);
    addCollectionExecutionCounters(stats, *collStats);
}

std::vector<tracking::shared_ptr<ExecutionStats>> releaseExecutionStatsFromBucketCatalog(
    BucketCatalog& catalog, std::span<const UUID> collectionUUIDs) {
    std::vector<tracking::shared_ptr<ExecutionStats>> out;
    out.reserve(collectionUUIDs.size());

    stdx::lock_guard catalogLock{catalog.mutex};
    for (auto&& uuid : collectionUUIDs) {
        auto it = catalog.executionStats.find(uuid);
        if (it != catalog.executionStats.end()) {
            out.push_back(std::move(it->second));
            catalog.executionStats.erase(it);
        }
    }

    return out;
}

Status getTimeseriesBucketClearedError(const OID& oid) {
    return {ErrorCodes::TimeseriesBucketCleared,
            str::stream() << "Time-series bucket " << oid << " was cleared"};
}

void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ExecutionStatsController& stats) {
    // Remove the bucket from the bucket state registry.
    stopTrackingBucketState(catalog.bucketStateRegistry, bucket.bucketId);

    removeBucket(catalog, stripe, stripeLock, bucket, stats, RemovalMode::kClose);
    return;
}

void closeArchivedBucket(BucketCatalog& catalog, const BucketId& bucketId) {
    // Remove the bucket from the bucket state registry.
    stopTrackingBucketState(catalog.bucketStateRegistry, bucketId);
}

std::variant<std::monostate, RolloverReason> insertBatchIntoEligibleBucket(
    OperationContext* opCtx,
    BucketCatalog& catalog,
    const Collection* bucketsColl,
    const StringDataComparator* comparator,
    const std::vector<BSONObj>& batchOfMeasurements,
    InsertContext& insertContext,
    Bucket& bucketToInsertInto,
    Stripe& stripe,
    WithLock stripeLock,
    size_t& currentPosition,
    std::vector<std::shared_ptr<WriteBatch>>& writeBatches,
    std::function<uint64_t(OperationContext*)> functionToGetStorageCacheBytes) {

    while (currentPosition < batchOfMeasurements.size()) {
        auto insertionResult = tryToInsertIntoBucketWithoutRollover(
            catalog,
            stripe,
            stripeLock,
            batchOfMeasurements[currentPosition],
            opCtx->getOpID(),
            bucket_catalog::internal::AllowBucketCreation::kNo,
            insertContext,
            bucketToInsertInto,
            batchOfMeasurements[currentPosition]
                .getField(bucketsColl->getTimeseriesOptions()->getTimeField())
                .Date(),
            functionToGetStorageCacheBytes(opCtx),
            comparator);
        if (auto* batch = std::get_if<std::shared_ptr<WriteBatch>>(&insertionResult)) {
            // Successfully inserted a measurement.
            ++currentPosition;
            writeBatches.emplace_back(*batch);
            continue;
        } else if (auto* rolloverReason = std::get_if<RolloverReason>(&insertionResult)) {
            // Return the reason for rollover to the caller, which must rollover the bucket.
            return *rolloverReason;
        }
    }
    return std::monostate{};
}

std::shared_ptr<WriteBatch> addMeasurementToBatchAndBucket(
    BucketCatalog& catalog,
    const BSONObj& doc,
    OperationId opId,
    InsertContext& insertContext,
    Bucket& bucket,
    const StringDataComparator* comparator,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    Sizes& sizesToBeAdded,
    bool isNewlyOpenedBucket,
    bool openedDueToMetadata) {
    auto batch = activeBatch(
        catalog.trackingContexts, bucket, opId, insertContext.stripeNumber, insertContext.stats);
    batch->measurements.push_back(doc);
    for (auto&& field : newFieldNamesToBeInserted) {
        batch->newFieldNamesToBeInserted[field] = field.hash();
        bucket.uncommittedFieldNames.emplace(tracking::StringMapHashedKey{
            getTrackingContext(catalog.trackingContexts, TrackingScope::kOpenBucketsById),
            field.key(),
            field.hash()});
    }

    bucket.numMeasurements++;
    batch->sizes.uncommittedMeasurementEstimate += sizesToBeAdded.uncommittedMeasurementEstimate;
    bucket.size +=
        sizesToBeAdded.uncommittedVerifiedSize + sizesToBeAdded.uncommittedMeasurementEstimate;
    if (isNewlyOpenedBucket) {
        if (openedDueToMetadata) {
            batch->openedDueToMetadata = true;
        }

        auto updateStatus =
            bucket.schema.update(doc, insertContext.options.getMetaField(), comparator);
        invariant(updateStatus == Schema::UpdateStatus::Updated);
    }

    return batch;
}

}  // namespace mongo::timeseries::bucket_catalog::internal
