//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/get_context.h"
#include "db/merge_helper.h"
#include "db/read_callback.h"
#include "monitoring/file_read_sample.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/statistics.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/statistics.h"
#include "util/util.h"

namespace rocksdb {

#ifndef ROCKSDB_LITE

namespace {

template <class T>
static void DeleteEntry(const Slice& /*key*/, void* value) {
  T* typed_value = reinterpret_cast<T*>(value);
  delete typed_value;
}

}

#endif  // ROCKSDB_LITE

GetContext::GetContext(const Comparator* ucmp,
                       const MergeOperator* merge_operator, Logger* logger,
                       Statistics* statistics, GetState init_state,
                       const Slice& user_key, LazySlice* lazy_val,
                       bool* value_found, MergeContext* merge_context,
                       const SeparateHelper* separate_helper,
                       SequenceNumber* _max_covering_tombstone_seq, Env* env,
                       SequenceNumber* seq, ReadCallback* callback)
    : ucmp_(ucmp),
      merge_operator_(merge_operator),
      logger_(logger),
      statistics_(statistics),
      state_(init_state),
      user_key_(user_key),
      lazy_val_(lazy_val),
      value_found_(value_found),
      merge_context_(merge_context),
      separate_helper_(separate_helper),
      max_covering_tombstone_seq_(_max_covering_tombstone_seq),
      env_(env),
      seq_(seq),
      min_seq_type_(0),
      callback_(callback) {
  if (seq_) {
    *seq_ = kMaxSequenceNumber;
  }
  sample_ = should_sample_file_read();
}

// Called from TableCache::Get and Table::Get when file/block in which
// key may exist are not there in TableCache/BlockCache respectively. In this
// case we can't guarantee that key does not exist and are not permitted to do
// IO to be certain.Set the status=kFound and value_found=false to let the
// caller know that key may exist but is not there in memory
void GetContext::MarkKeyMayExist() {
  state_ = kFound;
  if (value_found_ != nullptr) {
    *value_found_ = false;
  }
}

void GetContext::ReportCounters() {
  if (get_context_stats_.num_cache_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_HIT, get_context_stats_.num_cache_hit);
  }
  if (get_context_stats_.num_cache_index_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_HIT,
               get_context_stats_.num_cache_index_hit);
  }
  if (get_context_stats_.num_cache_data_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_HIT,
               get_context_stats_.num_cache_data_hit);
  }
  if (get_context_stats_.num_cache_filter_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_HIT,
               get_context_stats_.num_cache_filter_hit);
  }
  if (get_context_stats_.num_cache_index_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_MISS,
               get_context_stats_.num_cache_index_miss);
  }
  if (get_context_stats_.num_cache_filter_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_MISS,
               get_context_stats_.num_cache_filter_miss);
  }
  if (get_context_stats_.num_cache_data_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_MISS,
               get_context_stats_.num_cache_data_miss);
  }
  if (get_context_stats_.num_cache_bytes_read > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_READ,
               get_context_stats_.num_cache_bytes_read);
  }
  if (get_context_stats_.num_cache_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_MISS,
               get_context_stats_.num_cache_miss);
  }
  if (get_context_stats_.num_cache_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_ADD, get_context_stats_.num_cache_add);
  }
  if (get_context_stats_.num_cache_bytes_write > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_WRITE,
               get_context_stats_.num_cache_bytes_write);
  }
  if (get_context_stats_.num_cache_index_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_ADD,
               get_context_stats_.num_cache_index_add);
  }
  if (get_context_stats_.num_cache_index_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_BYTES_INSERT,
               get_context_stats_.num_cache_index_bytes_insert);
  }
  if (get_context_stats_.num_cache_data_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_ADD,
               get_context_stats_.num_cache_data_add);
  }
  if (get_context_stats_.num_cache_data_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_BYTES_INSERT,
               get_context_stats_.num_cache_data_bytes_insert);
  }
  if (get_context_stats_.num_cache_filter_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_ADD,
               get_context_stats_.num_cache_filter_add);
  }
  if (get_context_stats_.num_cache_filter_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_BYTES_INSERT,
               get_context_stats_.num_cache_filter_bytes_insert);
  }
}

bool GetContext::SaveValue(const ParsedInternalKey& parsed_key,
                           LazySlice&& value, bool* matched) {
  assert(matched);
  assert((state_ != kMerge && parsed_key.type != kTypeMerge &&
         parsed_key.type != kTypeMergeIndex) ||
         merge_context_ != nullptr || separate_helper_ == nullptr);
  if (ucmp_->Equal(parsed_key.user_key, user_key_)) {
    if (PackSequenceAndType(parsed_key.sequence, parsed_key.type) <
        min_seq_type_) {
      // for map sst, this key is masked
      return false;
    }
    *matched = true;
    // If the value is not in the snapshot, skip it
    if (!CheckCallback(parsed_key.sequence)) {
      return true;  // to continue to the next seq
    }

    if (seq_ != nullptr) {
      // Set the sequence number if it is uninitialized
      if (*seq_ == kMaxSequenceNumber) {
        *seq_ = parsed_key.sequence;
      }
    }

    auto type = parsed_key.type;
    // Key matches. Process it
    if ((type == kTypeValue || type == kTypeMerge || type == kTypeValueIndex ||
         type == kTypeMergeIndex) && max_covering_tombstone_seq_ != nullptr &&
        *max_covering_tombstone_seq_ > parsed_key.sequence) {
      type = kTypeRangeDeletion;
      value.clear();
    }
    switch (type) {
      case kTypeValueIndex:
        if (separate_helper_ == nullptr) {
          return false;
        }
        separate_helper_->TransToCombined(user_key_, parsed_key.sequence,
                                          value);
        // TODO modify for GC
        FALLTHROUGH_INTENDED;
      case kTypeValue:
        assert(state_ == kNotFound || state_ == kMerge);
        if (separate_helper_ == nullptr) {
          assert(kNotFound == state_);
          assert(lazy_val_ != nullptr);
          state_ = kFound;
          if (lazy_val_ != nullptr) {
            *lazy_val_ = std::move(value);
          }
          return false;
        }
        if (kNotFound == state_) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            value.decode_destructive(*lazy_val_);
          }
        } else if (kMerge == state_) {
          assert(merge_operator_ != nullptr);
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, &value,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
        }
        return false;

      case kTypeDeletion:
      case kTypeSingleDeletion:
      case kTypeRangeDeletion:
        // TODO(noetzli): Verify correctness once merge of single-deletes
        // is supported
        assert(state_ == kNotFound || state_ == kMerge);
        if (kNotFound == state_) {
          state_ = kDeleted;
        } else if (kMerge == state_) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
        }
        return false;

      case kTypeMergeIndex:
        if (separate_helper_ == nullptr) {
          return false;
        }
        separate_helper_->TransToCombined(user_key_, parsed_key.sequence,
                                          value);
        // TODO modify for GC
        FALLTHROUGH_INTENDED;
      case kTypeMerge:
        assert(state_ == kNotFound || state_ == kMerge);
        if (separate_helper_ == nullptr) {
          assert(kNotFound == state_);
          assert(lazy_val_ != nullptr);
          state_ = kMerge;
          if (lazy_val_ != nullptr) {
            *lazy_val_ = std::move(value);
          }
          return false;
        }
        state_ = kMerge;
        merge_context_->PushOperand(std::move(value));
        if (merge_operator_ != nullptr &&
            merge_operator_->ShouldMerge(
                merge_context_->GetOperandsDirectionBackward())) {
          state_ = kFound;
          if (LIKELY(lazy_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), lazy_val_, logger_, statistics_,
                env_);
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
            lazy_val_->pin_resource();
          }
          return false;
        }
        return true;

      default:
        assert(false);
        break;
    }
  }

  // state_ could be Corrupt, merge or notfound
  return false;
}

}  // namespace rocksdb
