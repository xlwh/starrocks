// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Inc.

#include "exec/pipeline/scan/scan_operator.h"

#include <util/time.h>

#include "column/chunk.h"
#include "exec/pipeline/chunk_accumulate_operator.h"
#include "exec/pipeline/limit_operator.h"
#include "exec/pipeline/pipeline_builder.h"
#include "exec/pipeline/scan/connector_scan_operator.h"
#include "exec/vectorized/olap_scan_node.h"
#include "exec/workgroup/scan_executor.h"
#include "exec/workgroup/work_group.h"
#include "runtime/current_thread.h"
#include "runtime/exec_env.h"
#include "util/debug/query_trace.h"
#include "util/runtime_profile.h"

namespace starrocks::pipeline {

// ========== ScanOperator ==========

ScanOperator::ScanOperator(OperatorFactory* factory, int32_t id, int32_t driver_sequence, int32_t dop,
                           ScanNode* scan_node)
        : SourceOperator(factory, id, scan_node->name(), scan_node->id(), driver_sequence),
          _scan_node(scan_node),
          _dop(dop),
          _io_tasks_per_scan_operator(scan_node->io_tasks_per_scan_operator()),
          _chunk_source_profiles(_io_tasks_per_scan_operator),
          _is_io_task_running(_io_tasks_per_scan_operator),
          _chunk_sources(_io_tasks_per_scan_operator) {
    for (auto i = 0; i < _io_tasks_per_scan_operator; i++) {
        _chunk_source_profiles[i] = std::make_shared<RuntimeProfile>(strings::Substitute("ChunkSource$0", i));
    }
}

ScanOperator::~ScanOperator() {
    auto* state = runtime_state();
    if (state == nullptr) {
        return;
    }

    for (size_t i = 0; i < _chunk_sources.size(); i++) {
        _close_chunk_source(state, i);
    }
}

Status ScanOperator::prepare(RuntimeState* state) {
    DCHECK(_scan_executor != nullptr);

    RETURN_IF_ERROR(SourceOperator::prepare(state));

    _unique_metrics->add_info_string("MorselQueueType", _morsel_queue->name());
    _unique_metrics->add_info_string("BufferUnplugThreshold", std::to_string(_buffer_unplug_threshold()));
    _peak_buffer_size_counter = _unique_metrics->AddHighWaterMarkCounter("PeakChunkBufferSize", TUnit::UNIT);
    _morsels_counter = ADD_COUNTER(_unique_metrics, "MorselsCount", TUnit::UNIT);
    _buffer_unplug_counter = ADD_COUNTER(_unique_metrics, "BufferUnplugCount", TUnit::UNIT);
    _submit_task_counter = ADD_COUNTER(_unique_metrics, "SubmitTaskCount", TUnit::UNIT);

    RETURN_IF_ERROR(do_prepare(state));

    return Status::OK();
}

void ScanOperator::close(RuntimeState* state) {
    set_buffer_finished();
    // For the running io task, we close its chunk sources in ~ScanOperator not in ScanOperator::close.
    for (size_t i = 0; i < _chunk_sources.size(); i++) {
        std::lock_guard guard(_task_mutex);
        if (!_is_io_task_running[i]) {
            _close_chunk_source_unlocked(state, i);
        }
    }

    _default_buffer_capacity_counter = ADD_COUNTER(_unique_metrics, "DefaultChunkBufferCapacity", TUnit::UNIT);
    COUNTER_SET(_default_buffer_capacity_counter, static_cast<int64_t>(default_buffer_capacity()));
    _buffer_capacity_counter = ADD_COUNTER(_unique_metrics, "ChunkBufferCapacity", TUnit::UNIT);
    COUNTER_SET(_buffer_capacity_counter, static_cast<int64_t>(buffer_capacity()));

    _tablets_counter = ADD_COUNTER(_unique_metrics, "TabletCount", TUnit::UNIT);
    COUNTER_SET(_tablets_counter, static_cast<int64_t>(_source_factory()->num_total_original_morsels()));

    _merge_chunk_source_profiles();

    do_close(state);
    Operator::close(state);
}

size_t ScanOperator::_buffer_unplug_threshold() const {
    size_t threshold = buffer_capacity() / _dop / 2;
    threshold = std::max<size_t>(1, std::min<size_t>(kIOTaskBatchSize, threshold));
    return threshold;
}

bool ScanOperator::has_output() const {
    if (_is_finished) {
        return false;
    }
    // if storage layer returns an error, we should make sure `pull_chunk` has a chance to get it
    if (!_get_scan_status().ok()) {
        return true;
    }

    // Try to buffer enough chunks for exec thread, to reduce scheduling overhead.
    // It's like the Linux Block-Scheduler's Unplug algorithm, so we just name it unplug.
    // The default threshould of unpluging is BufferCapacity/DOP/4, and its range is [1, 16]
    // The overall strategy:
    // 1. If enough buffered chunks: pull_chunk, so return true
    // 2. If not enough buffered chunks
    //   2.1 Enough running io-tasks and buffer not full: wait some time for more chunks, so return false
    //   2.1 Enough running io-tasks but buffer is full: pull_chunks, so return true
    //   2.2 Not enough running io-tasks: submit io tasks, so return true
    //   2.3 No more tasks: pull_chunk, so return true
    if (_unpluging) {
        if (num_buffered_chunks() > 0) {
            return true;
        }
        _unpluging = false;
    }
    if (num_buffered_chunks() >= _buffer_unplug_threshold()) {
        COUNTER_UPDATE(_buffer_unplug_counter, 1);
        _unpluging = true;
        return true;
    }
    if (is_buffer_full() && num_buffered_chunks() > 0) {
        return true;
    }
    if (_num_running_io_tasks >= _io_tasks_per_scan_operator || is_buffer_full()) {
        return false;
    }

    // Can pick up more morsels or submit more tasks
    if (!_morsel_queue->empty()) {
        return true;
    }
    for (int i = 0; i < _io_tasks_per_scan_operator; ++i) {
        std::shared_lock guard(_task_mutex);
        if (_chunk_sources[i] != nullptr && !_is_io_task_running[i] && _chunk_sources[i]->has_next_chunk()) {
            return true;
        }
    }

    return num_buffered_chunks() > 0;
}

bool ScanOperator::pending_finish() const {
    DCHECK(is_finished());
    return false;
}

bool ScanOperator::is_finished() const {
    if (_is_finished) {
        return true;
    }
    // if storage layer returns an error, we should make sure `pull_chunk` has a chance to get it
    if (!_get_scan_status().ok()) {
        return false;
    }

    // Any io task is running or needs to run.
    if (_num_running_io_tasks > 0 || !_morsel_queue->empty()) {
        return false;
    }

    // Any shared chunk source from other ScanOperator
    if (has_shared_chunk_source()) {
        return false;
    }

    // Remain some data in the buffer
    if (num_buffered_chunks() > 0) {
        return false;
    }

    // This scan operator is finished, if no more io tasks are running
    // or need to run, and all the read chunks are consumed.
    return true;
}

void ScanOperator::_detach_chunk_sources() {
    for (size_t i = 0; i < _chunk_sources.size(); i++) {
        detach_chunk_source(i);
    }
}

Status ScanOperator::set_finishing(RuntimeState* state) {
    std::lock_guard guard(_task_mutex);
    _detach_chunk_sources();
    _is_finished = true;
    return Status::OK();
}

StatusOr<vectorized::ChunkPtr> ScanOperator::pull_chunk(RuntimeState* state) {
    RETURN_IF_ERROR(_get_scan_status());

    _peak_buffer_size_counter->set(buffer_size());

    RETURN_IF_ERROR(_try_to_trigger_next_scan(state));

    vectorized::ChunkPtr res = get_chunk_from_buffer();
    if (res == nullptr) {
        return nullptr;
    }
    auto tablet_id = res->owner_info().owner_id();
    auto is_last_chunk = res->owner_info().is_last_chunk();
    eval_runtime_bloom_filters(res.get());
    res->owner_info().set_owner_id(tablet_id, is_last_chunk);
    return res;
}

int64_t ScanOperator::global_rf_wait_timeout_ns() const {
    const auto* global_rf_collector = runtime_bloom_filters();
    if (global_rf_collector == nullptr) {
        return 0;
    }

    return 1000'000L * global_rf_collector->scan_wait_timeout_ms();
}

Status ScanOperator::_try_to_trigger_next_scan(RuntimeState* state) {
    if (_num_running_io_tasks >= _io_tasks_per_scan_operator) {
        return Status::OK();
    }
    if (_unpluging && num_buffered_chunks() >= _buffer_unplug_threshold()) {
        return Status::OK();
    }

    // Avoid uneven distribution when io tasks execute very fast, so we start
    // traverse the chunk_source array from last visit idx
    int cnt = _io_tasks_per_scan_operator;
    while (--cnt >= 0) {
        _chunk_source_idx = (_chunk_source_idx + 1) % _io_tasks_per_scan_operator;
        int i = _chunk_source_idx;
        if (_is_io_task_running[i]) {
            continue;
        }

        if (_chunk_sources[i] != nullptr && _chunk_sources[i]->has_next_chunk()) {
            RETURN_IF_ERROR(_trigger_next_scan(state, i));
        } else {
            RETURN_IF_ERROR(_pickup_morsel(state, i));
        }
    }

    return Status::OK();
}

// this is a more efficient way to check if a weak_ptr has been initialized
// ref: https://stackoverflow.com/a/45507610
// after compiler optimization, it generates far fewer instructions than std::weak_ptr::expired() and std::weak_ptr::lock()
// see: https://godbolt.org/z/16bWqqM5n
inline bool is_uninitialized(const std::weak_ptr<QueryContext>& ptr) {
    using wp = std::weak_ptr<QueryContext>;
    return !ptr.owner_before(wp{}) && !wp{}.owner_before(ptr);
}

void ScanOperator::_close_chunk_source_unlocked(RuntimeState* state, int chunk_source_index) {
    if (_chunk_sources[chunk_source_index] != nullptr) {
        _chunk_sources[chunk_source_index]->close(state);
        _chunk_sources[chunk_source_index] = nullptr;
        detach_chunk_source(chunk_source_index);
    }
}

void ScanOperator::_close_chunk_source(RuntimeState* state, int chunk_source_index) {
    std::lock_guard guard(_task_mutex);
    _close_chunk_source_unlocked(state, chunk_source_index);
}

void ScanOperator::_finish_chunk_source_task(RuntimeState* state, int chunk_source_index, int64_t cpu_time_ns,
                                             int64_t scan_rows, int64_t scan_bytes) {
    _last_growth_cpu_time_ns += cpu_time_ns;
    _last_scan_rows_num += scan_rows;
    _last_scan_bytes += scan_bytes;
    _num_running_io_tasks--;

    DCHECK(_chunk_sources[chunk_source_index] != nullptr);

    {
        // - close() closes the chunk source which is not running.
        // - _finish_chunk_source_task() closes the chunk source conditionally and then make it as not running.
        // Therefore, closing chunk source and storing/loading `_is_finished` and `_is_io_task_running`
        // must be protected by lock
        std::lock_guard guard(_task_mutex);
        if (!_chunk_sources[chunk_source_index]->has_next_chunk() || _is_finished) {
            _close_chunk_source_unlocked(state, chunk_source_index);
        }

        _is_io_task_running[chunk_source_index] = false;
    }
}

Status ScanOperator::_trigger_next_scan(RuntimeState* state, int chunk_source_index) {
    ChunkBufferTokenPtr buffer_token;
    if (buffer_token = pin_chunk(1); buffer_token == nullptr) {
        return Status::OK();
    }

    COUNTER_UPDATE(_submit_task_counter, 1);
    _chunk_sources[chunk_source_index]->pin_chunk_token(std::move(buffer_token));
    _num_running_io_tasks++;
    _is_io_task_running[chunk_source_index] = true;

    // to avoid holding mutex in bthread, we choose to initialize lazily here instead of in prepare
    if (is_uninitialized(_query_ctx)) {
        _query_ctx = state->exec_env()->query_context_mgr()->get(state->query_id());
    }
    starrocks::debug::QueryTraceContext query_trace_ctx = starrocks::debug::tls_trace_ctx;
    query_trace_ctx.id = reinterpret_cast<int64_t>(_chunk_sources[chunk_source_index].get());
    int32_t driver_id = CurrentThread::current().get_driver_id();

    workgroup::ScanTask task;
    task.workgroup = _workgroup.get();
    // TODO: consider more factors, such as scan bytes and i/o time.
    task.priority = vectorized::OlapScanNode::compute_priority(_submit_task_counter->value());
    const auto io_task_start_nano = MonotonicNanos();
    task.work_function = [wp = _query_ctx, this, state, chunk_source_index, query_trace_ctx, driver_id,
                          io_task_start_nano]() {
        if (auto sp = wp.lock()) {
            // set driver_id/query_id/fragment_instance_id to thread local
            // driver_id will be used in some Expr such as regex_replace
            SCOPED_SET_TRACE_INFO(driver_id, state->query_id(), state->fragment_instance_id());
            SCOPED_THREAD_LOCAL_MEM_TRACKER_SETTER(state->instance_mem_tracker());

            [[maybe_unused]] std::string category = "chunk_source_" + std::to_string(chunk_source_index);
            QUERY_TRACE_ASYNC_START("io_task", category, query_trace_ctx);

            auto& chunk_source = _chunk_sources[chunk_source_index];

            DeferOp timer_defer([chunk_source]() {
                COUNTER_SET(chunk_source->scan_timer(),
                            chunk_source->io_task_wait_timer()->value() + chunk_source->io_task_exec_timer()->value());
            });
            COUNTER_UPDATE(chunk_source->io_task_wait_timer(), MonotonicNanos() - io_task_start_nano);
            SCOPED_TIMER(chunk_source->io_task_exec_timer());

            int64_t prev_cpu_time = chunk_source->get_cpu_time_spent();
            int64_t prev_scan_rows = chunk_source->get_scan_rows();
            int64_t prev_scan_bytes = chunk_source->get_scan_bytes();

            auto status = chunk_source->buffer_next_batch_chunks_blocking(state, kIOTaskBatchSize, _workgroup.get());
            if (!status.ok() && !status.is_end_of_file()) {
                _set_scan_status(status);
            }

            int64_t delta_cpu_time = chunk_source->get_cpu_time_spent() - prev_cpu_time;
            _finish_chunk_source_task(state, chunk_source_index, delta_cpu_time,
                                      chunk_source->get_scan_rows() - prev_scan_rows,
                                      chunk_source->get_scan_bytes() - prev_scan_bytes);

            QUERY_TRACE_ASYNC_FINISH("io_task", category, query_trace_ctx);
        }
    };

    if (_scan_executor->submit(std::move(task))) {
        _io_task_retry_cnt = 0;
    } else {
        _chunk_sources[chunk_source_index]->unpin_chunk_token();
        _num_running_io_tasks--;
        _is_io_task_running[chunk_source_index] = false;
        // TODO(hcf) set a proper retry times
        LOG(WARNING) << "ScanOperator failed to offer io task due to thread pool overload, retryCnt="
                     << _io_task_retry_cnt;
        if (++_io_task_retry_cnt > 100) {
            return Status::RuntimeError("ScanOperator failed to offer io task due to thread pool overload");
        }
    }

    return Status::OK();
}

Status ScanOperator::_pickup_morsel(RuntimeState* state, int chunk_source_index) {
    DCHECK(_morsel_queue != nullptr);
    _close_chunk_source(state, chunk_source_index);

    // NOTE: attach an active source before really creating it, to avoid the race condition
    bool need_detach = true;
    attach_chunk_source(chunk_source_index);
    DeferOp defer([&]() {
        if (need_detach) {
            detach_chunk_source(chunk_source_index);
        }
    });

    ASSIGN_OR_RETURN(auto morsel, _morsel_queue->try_get());

    if (_lane_arbiter != nullptr) {
        while (morsel != nullptr) {
            auto [lane_owner, version] = morsel->get_lane_owner_and_version();
            auto acquire_result = _lane_arbiter->try_acquire_lane(lane_owner);
            if (acquire_result == query_cache::AR_BUSY) {
                _morsel_queue->unget(std::move(morsel));
                return Status::OK();
            } else if (acquire_result == query_cache::AR_PROBE) {
                auto hit = _cache_operator->probe_cache(lane_owner, version);
                RETURN_IF_ERROR(_cache_operator->reset_lane(lane_owner));
                if (hit) {
                    ASSIGN_OR_RETURN(morsel, _morsel_queue->try_get());
                } else {
                    break;
                }
            } else if (acquire_result == query_cache::AR_SKIP) {
                ASSIGN_OR_RETURN(morsel, _morsel_queue->try_get());
            } else if (acquire_result == query_cache::AR_IO) {
                break;
            }
        }
    }

    if (morsel != nullptr) {
        COUNTER_UPDATE(_morsels_counter, 1);

        _chunk_sources[chunk_source_index] = create_chunk_source(std::move(morsel), chunk_source_index);
        auto status = _chunk_sources[chunk_source_index]->prepare(state);
        if (!status.ok()) {
            _chunk_sources[chunk_source_index] = nullptr;
            set_finishing(state);
            return status;
        }
        need_detach = false;
        RETURN_IF_ERROR(_trigger_next_scan(state, chunk_source_index));
    }

    return Status::OK();
}

void ScanOperator::_merge_chunk_source_profiles() {
    std::vector<RuntimeProfile*> profiles(_chunk_source_profiles.size());
    for (auto i = 0; i < _chunk_source_profiles.size(); i++) {
        profiles[i] = _chunk_source_profiles[i].get();
    }
    RuntimeProfile::merge_isomorphic_profiles(profiles);

    RuntimeProfile* merged_profile = profiles[0];

    _unique_metrics->copy_all_info_strings_from(merged_profile);
    _unique_metrics->copy_all_counters_from(merged_profile);
}

// ========== ScanOperatorFactory ==========

ScanOperatorFactory::ScanOperatorFactory(int32_t id, ScanNode* scan_node)
        : SourceOperatorFactory(id, scan_node->name(), scan_node->id()), _scan_node(scan_node) {}

Status ScanOperatorFactory::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(OperatorFactory::prepare(state));
    RETURN_IF_ERROR(do_prepare(state));

    return Status::OK();
}

OperatorPtr ScanOperatorFactory::create(int32_t degree_of_parallelism, int32_t driver_sequence) {
    return do_create(degree_of_parallelism, driver_sequence);
}

void ScanOperatorFactory::close(RuntimeState* state) {
    do_close(state);

    OperatorFactory::close(state);
}

// ====================

pipeline::OpFactories decompose_scan_node_to_pipeline(std::shared_ptr<ScanOperatorFactory> scan_operator,
                                                      ScanNode* scan_node, pipeline::PipelineBuilderContext* context) {
    OpFactories ops;

    const auto* morsel_queue_factory = context->morsel_queue_factory_of_source_operator(scan_operator.get());
    scan_operator->set_degree_of_parallelism(morsel_queue_factory->size());
    scan_operator->set_need_local_shuffle(morsel_queue_factory->need_local_shuffle());

    ops.emplace_back(std::move(scan_operator));

    if (!scan_node->conjunct_ctxs().empty() || ops.back()->has_runtime_filters()) {
        ops.emplace_back(
                std::make_shared<ChunkAccumulateOperatorFactory>(context->next_operator_id(), scan_node->id()));
    }

    size_t limit = scan_node->limit();
    if (limit != -1) {
        ops.emplace_back(
                std::make_shared<pipeline::LimitOperatorFactory>(context->next_operator_id(), scan_node->id(), limit));
    }

    return ops;
}
} // namespace starrocks::pipeline
