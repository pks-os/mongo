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

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"
#include "mongo/db/timeseries/bucket_catalog/closed_bucket.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/db/timeseries/bucket_catalog/reopening.h"
#include "mongo/db/timeseries/bucket_catalog/rollover.h"
#include "mongo/db/timeseries/bucket_catalog/write_batch.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/time_support.h"

namespace mongo::timeseries::bucket_catalog::internal {

/**
 * Mode enum to control whether bucket retrieval methods will create new buckets if no suitable
 * bucket exists.
 */
enum class AllowBucketCreation { kYes, kNo };

/**
 * Mode to signal to 'removeBucket' what's happening to the bucket, and how to handle the bucket
 * state change.
 */
enum class RemovalMode {
    kClose,    // Normal closure
    kArchive,  // Archive bucket, no state change
    kAbort,    // Bucket is being cleared, possibly due to error, erase state
};

/**
 * Mode enum to control whether the bucket retrieval methods will return buckets that have a state
 * that conflicts with insertion.
 */
enum class IgnoreBucketState { kYes, kNo };

/**
 * Mode enum to control whether we prepare or unprepare a bucket.
 */
enum class BucketPrepareAction { kPrepare, kUnprepare };

/**
 * Mode enum to control whether getReopeningCandidate() will allow query-based
 * reopening of buckets when attempting to accommodate a new measurement.
 */
enum class AllowQueryBasedReopening { kAllow, kDisallow };

/**
 * Maps bucket identifier to the stripe that is responsible for it.
 */
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketKey& key);
StripeNumber getStripeNumber(const BucketCatalog& catalog, const BucketId& bucketId);

/**
 * Extracts the information from the input 'doc' that is used to map the document to a bucket.
 */
StatusWith<std::pair<BucketKey, Date_t>> extractBucketingParameters(
    tracking::Context&,
    const UUID& collectionUUID,
    const TimeseriesOptions& options,
    const BSONObj& doc);


/**
 * Retrieve a bucket for read-only use.
 */
const Bucket* findBucket(BucketStateRegistry& registry,
                         const Stripe& stripe,
                         WithLock stripeLock,
                         const BucketId& bucketId,
                         IgnoreBucketState mode = IgnoreBucketState::kNo);

/**
 * Retrieve a bucket for write use.
 */
Bucket* useBucket(BucketStateRegistry& registry,
                  Stripe& stripe,
                  WithLock stripeLock,
                  const BucketId& bucketId,
                  IgnoreBucketState mode);

/**
 * Retrieve a bucket for write use and prepare/unprepare the 'BucketState'.
 */
Bucket* useBucketAndChangePreparedState(BucketStateRegistry& registry,
                                        Stripe& stripe,
                                        WithLock stripeLock,
                                        const BucketId& bucketId,
                                        BucketPrepareAction prepare);

/**
 * Retrieve the open bucket for write use if one exists. If none exists and 'mode' is set to kYes,
 * then we will create a new bucket.
 */
Bucket* useBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  InsertContext& info,
                  AllowBucketCreation mode,
                  const Date_t& time,
                  const StringDataComparator* comparator);

/**
 * Retrieve a previously closed bucket for write use if one exists in the catalog. Considers buckets
 * that are pending closure or archival but which are still eligible to receive new measurements.
 */
Bucket* useAlternateBucket(BucketCatalog& catalog,
                           Stripe& stripe,
                           WithLock stripeLock,
                           InsertContext& info,
                           const Date_t& time);

/**
 * Given a bucket to reopen, performs validation and constructs the in-memory representation of the
 * bucket. If specified, 'expectedKey' is matched against the key extracted from the document to
 * validate that the bucket is expected (i.e. to help resolve hash collisions for archived buckets).
 * Does *not* hand ownership of the bucket to the catalog.
 */
StatusWith<tracking::unique_ptr<Bucket>> rehydrateBucket(BucketCatalog& catalog,
                                                         ExecutionStatsController& stats,
                                                         const UUID& collectionUUID,
                                                         const StringDataComparator* comparator,
                                                         const TimeseriesOptions& options,
                                                         const BucketToReopen& bucketToReopen,
                                                         uint64_t catalogEra,
                                                         const BucketKey* expectedKey);

/**
 * Given a rehydrated 'bucket', passes ownership of that bucket to the catalog, marking the bucket
 * as open.
 */
StatusWith<std::reference_wrapper<Bucket>> reopenBucket(BucketCatalog& catalog,
                                                        Stripe& stripe,
                                                        WithLock stripeLock,
                                                        ExecutionStatsController& stats,
                                                        const BucketKey& key,
                                                        tracking::unique_ptr<Bucket>&& bucket,
                                                        std::uint64_t targetEra,
                                                        ClosedBuckets& closedBuckets);

/**
 * Check to see if 'insert' can use existing bucket rather than reopening a candidate bucket. If
 * true, chances are the caller raced with another thread to reopen the same bucket, but if false,
 * there might be another bucket that had been cleared, or that has the same _id in a different
 * namespace.
 */
StatusWith<std::reference_wrapper<Bucket>> reuseExistingBucket(BucketCatalog& catalog,
                                                               Stripe& stripe,
                                                               WithLock stripeLock,
                                                               ExecutionStatsController& stats,
                                                               const BucketKey& key,
                                                               Bucket& existingBucket,
                                                               std::uint64_t targetEra);

/**
 * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, and 'mode'
 * is set to 'kYes', we will create a new bucket and insert into that bucket. If `existingBucket`
 * was selected via `useAlternateBucket`, then the previous bucket returned by `useBucket` should be
 * passed in as `excludedBucket`.
 */
std::variant<std::shared_ptr<WriteBatch>, RolloverReason> insertIntoBucket(
    BucketCatalog& catalog,
    Stripe& stripe,
    WithLock stripeLock,
    const BSONObj& doc,
    OperationId,
    AllowBucketCreation mode,
    InsertContext& insertContext,
    Bucket& existingBucket,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator,
    Bucket* excludedBucket = nullptr,
    boost::optional<RolloverAction> excludedAction = boost::none);

/**
 * Wait for other batches to finish so we can prepare 'batch'
 */
void waitToCommitBatch(BucketStateRegistry& registry,
                       Stripe& stripe,
                       const std::shared_ptr<WriteBatch>& batch);

/**
 * Removes the given bucket from the bucket catalog's internal data structures.
 */
void removeBucket(BucketCatalog& catalog,
                  Stripe& stripe,
                  WithLock stripeLock,
                  Bucket& bucket,
                  ExecutionStatsController& stats,
                  RemovalMode mode);

/**
 * Archives the given bucket, minimizing the memory footprint but retaining the necessary
 * information required to efficiently identify it as a candidate for future insertions.
 */
void archiveBucket(BucketCatalog& catalog,
                   Stripe& stripe,
                   WithLock stripeLock,
                   Bucket& bucket,
                   ExecutionStatsController& stats,
                   ClosedBuckets& closedBuckets);

/**
 * Identifies a previously archived bucket that may be able to accommodate the measurement
 * represented by 'info', if one exists.
 */
boost::optional<OID> findArchivedCandidate(
    BucketCatalog& catalog, Stripe& stripe, WithLock stripeLock, InsertContext& info, Date_t time);

/**
 * Calculates the bucket max size constrained by the cache size and the cardinality of active
 * buckets. Returns a pair of the effective value that respects the absolute bucket max and min
 * sizes and the raw value.
 */
std::pair<int32_t, int32_t> getCacheDerivedBucketMaxSize(uint64_t storageCacheSizeBytes,
                                                         uint32_t workloadCardinality);

/**
 * Identifies a previously archived bucket that may be able to accommodate the measurement
 * represented by 'info', if one exists. Otherwise returns a pipeline to use for query-based
 * reopening if allowed.
 */
InsertResult getReopeningContext(BucketCatalog& catalog,
                                 Stripe& stripe,
                                 WithLock stripeLock,
                                 InsertContext& info,
                                 uint64_t catalogEra,
                                 AllowQueryBasedReopening allowQueryBasedReopening,
                                 const Date_t& time,
                                 uint64_t storageCacheSizeBytes);

/**
 * Aborts 'batch', and if the corresponding bucket still exists, proceeds to abort any other
 * unprepared batches and remove the bucket from the catalog if there is no unprepared batch.
 */
void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           std::shared_ptr<WriteBatch> batch,
           const Status& status);

/**
 * Aborts any unprepared batches for the given bucket, then removes the bucket if there is no
 * prepared batch.
 */
void abort(BucketCatalog& catalog,
           Stripe& stripe,
           WithLock stripeLock,
           Bucket& bucket,
           ExecutionStatsController& stats,
           std::shared_ptr<WriteBatch> batch,
           const Status& status);

/**
 * Adds the bucket to a list of idle buckets to be expired at a later date.
 */
void markBucketIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket);

/**
 * Remove the bucket from the list of idle buckets. The second parameter encodes whether the caller
 * holds a lock on _idleMutex.
 */
void markBucketNotIdle(Stripe& stripe, WithLock stripeLock, Bucket& bucket);

/**
 * Expires idle buckets until the bucket catalog's memory usage is below the expiry threshold.
 */
void expireIdleBuckets(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       const UUID& collectionUUID,
                       ExecutionStatsController& collectioStats,
                       ClosedBuckets& closedBuckets);

/**
 * Generates an OID for the bucket _id field, setting the timestamp portion to a value determined by
 * rounding 'time' based on 'options'.
 */
std::pair<OID, Date_t> generateBucketOID(const Date_t& time, const TimeseriesOptions& options);

/**
 * Resets the counter used for bucket OID generation. Should be called after a collision.
 */
void resetBucketOIDCounter();

/**
 * Allocates a new bucket and adds it to the catalog.
 */
Bucket& allocateBucket(BucketCatalog& catalog,
                       Stripe& stripe,
                       WithLock stripeLock,
                       InsertContext& info,
                       const Date_t& time,
                       const StringDataComparator* comparator);

/**
 * Close the existing, full bucket and open a new one for the same metadata.
 *
 * Writes information about the closed bucket to the 'info' parameter. Optionally, if `bucket` was
 * selected via `useAlternateBucket`, pass the current open bucket as `additionalBucket` to mark for
 * archival and preserve the invariant of only one open bucket per key.
 */
Bucket& rollover(BucketCatalog& catalog,
                 Stripe& stripe,
                 WithLock stripeLock,
                 Bucket& bucket,
                 InsertContext& info,
                 RolloverAction action,
                 const Date_t& time,
                 const StringDataComparator* comparator,
                 Bucket* additionalBucket,
                 boost::optional<RolloverAction> additionalAction);

/**
 * Determines if 'bucket' needs to be rolled over to accommodate 'doc'. If so, determines whether
 * to archive or close 'bucket'.
 */
std::pair<RolloverAction, RolloverReason> determineRolloverAction(
    TrackingContexts&,
    const BSONObj& doc,
    InsertContext& info,
    Bucket& bucket,
    uint32_t numberOfActiveBuckets,
    Bucket::NewFieldNames& newFieldNamesToBeInserted,
    Sizes& sizesToBeAdded,
    AllowBucketCreation mode,
    const Date_t& time,
    uint64_t storageCacheSizeBytes,
    const StringDataComparator* comparator);

/**
 * Retrieves or initializes the execution stats for the given namespace, for writing.
 */
ExecutionStatsController getOrInitializeExecutionStats(BucketCatalog& catalog,
                                                       const UUID& collectionUUID);

/**
 * Retrieves the execution stats for the given namespace, if they have already been initialized. A
 * valid instance is returned if the stats does not exist for the given namespace but any statistics
 * reported to it will not be reported to the catalog.
 */
ExecutionStatsController getExecutionStats(BucketCatalog& catalog, const UUID& collectionUUID);

/**
 * Retrieves the execution stats for the given namespace, if they have already been initialized.
 */
tracking::shared_ptr<ExecutionStats> getCollectionExecutionStats(const BucketCatalog& catalog,
                                                                 const UUID& collectionUUID);

/**
 * Release the execution stats of a collection from the bucket catalog.
 */
std::vector<tracking::shared_ptr<ExecutionStats>> releaseExecutionStatsFromBucketCatalog(
    BucketCatalog& catalog, std::span<const UUID> collectionUUIDs);

/**
 * Retrieves the execution stats from the side bucket catalog.
 * Assumes the side bucket catalog has the stats of one collection.
 */
std::pair<UUID, tracking::shared_ptr<ExecutionStats>> getSideBucketCatalogCollectionStats(
    BucketCatalog& sideBucketCatalog);

/**
 * Merges the execution stats of a collection into the bucket catalog.
 */
void mergeExecutionStatsToBucketCatalog(BucketCatalog& catalog,
                                        tracking::shared_ptr<ExecutionStats> collStats,
                                        const UUID& collectionUUID);

/**
 * Generates a status with code TimeseriesBucketCleared and an appropriate error message.
 */
Status getTimeseriesBucketClearedError(const OID& oid);

/**
 * Close an open bucket, setting the state appropriately and removing it from the catalog.
 */
void closeOpenBucket(BucketCatalog& catalog,
                     Stripe& stripe,
                     WithLock stripeLock,
                     Bucket& bucket,
                     ExecutionStatsController& stats);

/**
 * Close an archived bucket, setting the state appropriately and removing it from the catalog.
 */
void closeArchivedBucket(BucketCatalog& catalog, const BucketId& bucketId);

/**
 * Inserts measurements into the provided eligible bucket. On success of all measurements being
 * inserted into the provided bucket, returns std::monostate. Otherwise, returns the rollover reason
 * along with the index of the measurement 'currentPosition' where insertion stopped.
 */
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
    std::function<uint64_t(OperationContext*)> functionToGetStorageCacheBytes);

/**
 * Given an already-selected 'bucket', inserts 'doc' to the bucket if possible. If not, we return
 * the reason for why attempting to insert the measurement into the bucket would result in the
 * bucket being rolled over.
 */
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
    const StringDataComparator* comparator);

/**
 * Given a bucket 'bucket' and a measurement 'doc', updates the WriteBatch corresponding to the
 * inputted bucket as well as the bucket itself to reflect the addition of the measurement. This
 * includes updating the batch/bucket estimated sizes and the bucket's schema.
 * Returns the WriteBatch for the bucket.
 */
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
    bool openedDueToMetadata);

}  // namespace mongo::timeseries::bucket_catalog::internal
