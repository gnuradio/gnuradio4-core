#ifndef GNURADIO_FUSED_RUN_HPP
#define GNURADIO_FUSED_RUN_HPP

#include <bit>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/Graph.hpp>

namespace gr::fusion {

using RunMembers = std::vector<std::shared_ptr<BlockModel>>;

inline constexpr std::size_t kL2Budget = 256UZ * 1024UZ; // conservative shared floor, not a probe of the host
inline constexpr std::size_t kMinChunk = 256UZ;
inline constexpr std::size_t kMaxChunk = 65536UZ;

enum class StageKind { none, composed, bulk };

/**
 * @brief Whether a block may be a member of a fused run, and how the run would drive it.
 *
 * `composed` is driven from a scratch buffer: `fusedStage()` carries the compile-time facts (processOne rather than
 * processBulk, default tag propagation, no stride or resampling) and the ratio is re-checked here. `bulk` is driven
 * through the block's own `work()`, so any tag policy and any constant L:M ratio is admissible. Everything else is
 * read from the type-erased interface: port synchronicity comes from blockInputTypes()/blockOutputTypes(), which
 * report whether an async port exists rather than whether one currently holds samples. Port connectedness is read
 * from the graph's edge list by planFusedRuns(), the only record of it at plan time, since planning runs before
 * the scheduler connects the pending edges.
 */
[[nodiscard]] inline StageKind stageKindOf(BlockModel& block) {
    if (block.blockCategory() != block::Category::NormalBlock) {
        return StageKind::none;
    }
    const std::span<const port::BitMask> inputs  = block.blockInputTypes();
    const std::span<const port::BitMask> outputs = block.blockOutputTypes();
    if (inputs.size() != 1UZ || outputs.size() != 1UZ) {
        return StageKind::none;
    }
    if (!port::isSynchronous(inputs[0]) || !port::isSynchronous(outputs[0])) {
        return StageKind::none;
    }
    if (block.stride() != 0U) {
        return StageKind::none;
    }
    const gr::Ratio ratio = block.resamplingRatio();
    if (block.fusedStage() != nullptr) {
        return ratio.numerator == 1 && ratio.denominator == 1 ? StageKind::composed : StageKind::none;
    }
    if (block.bulkStage() != nullptr && ratio.numerator >= 1 && ratio.denominator >= 1) {
        return StageKind::bulk;
    }
    return StageKind::none;
}

/// one visit of a fused run: a composed segment driven over scratch, or one processBulk member driven through work()
struct Stage {
    std::size_t firstMember{};
    std::size_t nMembers{};
    bool        isComposed{};
    gr::Ratio   ratio{1, 1};    ///< the planned L:M, re-read once per chunk
    std::size_t inputSamples{}; ///< what this stage consumes for one run chunk
};

/// an edge between two stages: real, cache-resident, and restored to its recorded size when the plan dissolves
struct InteriorEdge {
    Edge        edge; ///< a copy taken from the flattened graph, resolved to the live edge before it is written
    std::size_t samples{};
    std::size_t previousMinBufferSize{undefined_size}; ///< undefined_size until the planner has written the live edge
};

struct RunPlan {
    RunMembers                members;
    std::vector<Stage>        stages;
    std::vector<InteriorEdge> interiorEdges; ///< interiorEdges[k] follows stages[k], so there is one fewer than stages
    std::size_t               quantum          = 1UZ;
    std::size_t               outputPerQuantum = 1UZ;
    std::size_t               chunkSamples     = 0UZ;

    [[nodiscard]] bool hasBulkStage() const noexcept {
        return std::ranges::any_of(stages, [](const Stage& stage) { return !stage.isComposed; });
    }
};

namespace detail {

[[nodiscard]] inline std::vector<Stage> splitIntoStages(const RunMembers& members) {
    std::vector<Stage> stages;
    for (std::size_t i = 0UZ; i < members.size();) {
        const block::FusedStage* fused = members[i]->fusedStage();
        if (fused == nullptr) {
            stages.push_back(Stage{i, 1UZ, false, members[i]->resamplingRatio(), 0UZ});
            ++i;
            continue;
        }
        std::size_t n = 1UZ; // a non-pure processOne ends the segment: an early break would leave earlier members over-advanced
        while (members[i + n - 1UZ]->fusedStage()->isPure && i + n < members.size() && members[i + n]->fusedStage() != nullptr) {
            ++n;
        }
        stages.push_back(Stage{i, n, true, gr::Ratio{1, 1}, 0UZ});
        i += n;
    }
    return stages;
}

/// the smallest run input count that gives every stage a whole number of input samples that is a multiple of its L
[[nodiscard]] inline bool computeQuantum(std::vector<Stage>& stages, std::size_t& quantum, std::size_t& outputPerQuantum) {
    quantum       = 1UZ;
    std::size_t n = 1UZ;
    for (std::size_t k = 0UZ; k < stages.size(); ++k) {
        const std::size_t nIn   = static_cast<std::size_t>(stages[k].ratio.numerator);
        const std::size_t nOut  = static_cast<std::size_t>(stages[k].ratio.denominator);
        const std::size_t scale = nIn / std::gcd(n, nIn);
        if (scale != 0UZ && quantum > kMaxChunk / scale) {
            stages.resize(k);
            return false;
        }
        quantum *= scale;
        n *= scale;
        for (std::size_t j = 0UZ; j < k; ++j) {
            stages[j].inputSamples *= scale;
        }
        stages[k].inputSamples = n;
        n                      = n / nIn * nOut;
    }
    outputPerQuantum = n;
    return true;
}

[[nodiscard]] inline std::size_t valueSizeOut(const std::shared_ptr<BlockModel>& member) {
    const block::FusedStage* fused = member->fusedStage();
    return fused != nullptr ? fused->valueSizeOut : member->bulkStage()->valueSizeOut;
}

[[nodiscard]] inline std::size_t valueSizeIn(const std::shared_ptr<BlockModel>& member) {
    const block::FusedStage* fused = member->fusedStage();
    return fused != nullptr ? fused->valueSizeIn : member->bulkStage()->valueSizeIn;
}

} // namespace detail

[[nodiscard]] inline std::size_t chunkSamples(const RunPlan& plan, std::size_t requestedChunkSamples = 0UZ) {
    std::size_t requested = requestedChunkSamples;
    if (requested == 0UZ) {
        std::size_t maxValueSize = 0UZ; // only a composed segment uses scratch, so only its members size it
        for (const Stage& stage : plan.stages) {
            for (std::size_t k = 0UZ; stage.isComposed && k < stage.nMembers; ++k) {
                const block::FusedStage* fused = plan.members[stage.firstMember + k]->fusedStage();
                maxValueSize                   = std::max({maxValueSize, fused->valueSizeIn, fused->valueSizeOut});
            }
        }
        // the run's input window plus the two scratch buffers; the output window is written streaming
        const std::size_t perSample = detail::valueSizeIn(plan.members.front()) + 2UZ * maxValueSize;
        requested                   = std::bit_floor(std::clamp(kL2Budget / perSample, kMinChunk, kMaxChunk));
    }
    return std::max(plan.quantum, plan.quantum * (requested / plan.quantum));
}

/**
 * @brief The ring size for the edge after stage `k`, in samples.
 *
 * The first term is the target: 256 KiB of the edge's value type, which is where a full-rate edge stops being a walk
 * through DRAM. The other two are correctness floors -- a ring at or above them still lets the unfused path make
 * progress, just more slowly, until the next init()/reset() restores the recorded size. The second holds a whole
 * chunk while an earlier short consumption is still in the ring; the third keeps the ring well clear of the
 * consumer's own chunk floor so a tag-dense stream degrades rather than stalls.
 */
[[nodiscard]] inline std::size_t interiorEdgeSamples(const RunPlan& plan, std::size_t stageIndex, std::size_t requestedSamples = 0UZ) {
    const Stage&      producer     = plan.stages[stageIndex];
    const Stage&      consumer     = plan.stages[stageIndex + 1UZ];
    const std::size_t elementSize  = detail::valueSizeOut(plan.members[producer.firstMember + producer.nMembers - 1UZ]);
    const std::size_t target       = requestedSamples != 0UZ ? requestedSamples : kL2Budget / elementSize;
    const auto        minimumInput = plan.members[consumer.firstMember]->minInputRequirements();

    const std::size_t chunkFloor = 2UZ * consumer.inputSamples + (minimumInput.empty() ? 0UZ : minimumInput[0]);
    const std::size_t ratioFloor = 4UZ * static_cast<std::size_t>(consumer.ratio.numerator);
    return std::bit_ceil(std::max({target, chunkFloor, ratioFloor}));
}

/**
 * @brief Maximal chains of fusable blocks that share one job, one per job of the scheduler's execution order.
 *
 * A run is extended while the interior edge is the only outgoing edge of the producer and the only incoming edge of
 * the consumer, both ends carry the same value type, and both blocks land in the same job. A member with a non-const
 * processOne ends its composed segment rather than the run, and a processBulk member is always a segment of its own,
 * so a run holds at most one stateful composed member per segment and any number of stateful bulk members.
 */
[[nodiscard]] inline std::vector<std::vector<RunPlan>> planFusedRuns(const gr::Graph& flatGraph, const std::vector<std::vector<std::shared_ptr<BlockModel>>>& jobs, std::size_t requestedChunkSamples = 0UZ, std::size_t requestedInteriorSamples = 0UZ) {
    using BlockPtr = std::shared_ptr<BlockModel>;

    std::unordered_map<const BlockModel*, std::size_t> nOutgoing;
    std::unordered_map<const BlockModel*, std::size_t> nIncoming;
    std::unordered_map<const BlockModel*, const Edge*> outgoingEdge;
    for (const Edge& edge : flatGraph.edges()) {
        nOutgoing[edge.sourceBlock().get()]++;
        nIncoming[edge.destinationBlock().get()]++;
        outgoingEdge[edge.sourceBlock().get()] = std::addressof(edge);
    }

    // a member needs a stream to read and a stream to write; before connectPendingEdges() the edge list is the only record of that
    const auto isRunMember = [&](const BlockPtr& block) {
        const auto inIt  = nIncoming.find(block.get());
        const auto outIt = nOutgoing.find(block.get());
        return inIt != nIncoming.end() && outIt != nOutgoing.end() && stageKindOf(*block) != StageKind::none;
    };

    std::vector<std::vector<RunPlan>> plan(jobs.size());
    for (std::size_t jobIndex = 0UZ; jobIndex < jobs.size(); ++jobIndex) {
        std::unordered_set<const BlockModel*> jobMembers;
        for (const BlockPtr& block : jobs[jobIndex]) {
            jobMembers.insert(block.get());
        }

        const auto successor = [&](const BlockPtr& from) -> BlockPtr {
            const auto outIt = nOutgoing.find(from.get());
            if (outIt == nOutgoing.end() || outIt->second != 1UZ) {
                return nullptr;
            }
            const Edge*     edge = outgoingEdge.at(from.get());
            const BlockPtr& to   = edge->destinationBlock();
            if (!jobMembers.contains(to.get()) || nIncoming.at(to.get()) != 1UZ || !isRunMember(to)) {
                return nullptr;
            }
            const auto sourcePort      = from->dynamicOutputPort(edge->sourcePortDefinition());
            const auto destinationPort = to->dynamicInputPort(edge->destinationPortDefinition());
            if (!sourcePort.has_value() || !destinationPort.has_value()) {
                return nullptr;
            }
            if (sourcePort.value()->typeName() != destinationPort.value()->typeName()) {
                return nullptr;
            }
            return to;
        };

        std::unordered_set<const BlockModel*> extendedInto;
        for (const BlockPtr& block : jobs[jobIndex]) {
            if (!isRunMember(block)) {
                continue;
            }
            if (const BlockPtr next = successor(block); next != nullptr) {
                extendedInto.insert(next.get());
            }
        }

        for (const BlockPtr& block : jobs[jobIndex]) {
            if (!isRunMember(block) || extendedInto.contains(block.get())) {
                continue;
            }
            RunMembers members{block};
            for (BlockPtr current = block;;) {
                const BlockPtr next = successor(current);
                if (next == nullptr || std::ranges::find(members, next) != members.end()) {
                    break;
                }
                members.push_back(next);
                current = next;
            }

            RunPlan run;
            run.members = std::move(members);
            run.stages  = detail::splitIntoStages(run.members);
            std::ignore = detail::computeQuantum(run.stages, run.quantum, run.outputPerQuantum); // a refused quantum truncates the run
            if (run.stages.empty()) {
                continue;
            }
            run.members.resize(run.stages.back().firstMember + run.stages.back().nMembers);
            if (run.members.size() < 2UZ) {
                continue;
            }

            run.chunkSamples = chunkSamples(run, requestedChunkSamples);
            for (Stage& stage : run.stages) {
                stage.inputSamples *= run.chunkSamples / run.quantum;
            }
            for (std::size_t k = 0UZ; k + 1UZ < run.stages.size(); ++k) {
                const BlockPtr& producer = run.members[run.stages[k].firstMember + run.stages[k].nMembers - 1UZ];
                run.interiorEdges.push_back(InteriorEdge{*outgoingEdge.at(producer.get()), interiorEdgeSamples(run, k, requestedInteriorSamples), undefined_size});
            }
            plan[jobIndex].push_back(std::move(run));
        }
    }
    return plan;
}

/**
 * @brief One fused run, presented to the scheduler as a single schedulable unit.
 *
 * The members keep their ports and their settings; only their `work()` loop is replaced. A composed segment is read
 * from its first member's input port, driven over ping-pong scratch buffers and written to its last member's output
 * port, so the edges inside it never carry a sample. Tags and staged settings are applied per member, in run order,
 * once per chunk, before any sample of that chunk is touched.
 */
struct FusedRun : BlockModel {
    using ScratchAllocator = gr::allocator::Aligned<std::byte, gr::kCacheLine>;

    RunMembers                            _members;
    std::vector<Stage>                    _stages;
    std::vector<const block::FusedStage*> _fusedStages; ///< per member, nullptr for a bulk member
    std::shared_ptr<gr::Sequence>         _progress;
    std::string                           _name;
    std::string                           _uniqueName;
    std::string                           _typeName = "gr::fusion::FusedRun";

    std::size_t                              _chunkSamples;
    std::size_t                              _quantum;
    std::size_t                              _outputPerQuantum;
    std::size_t                              _scratchStride;
    bool                                     _inPlaceScratch;
    std::vector<std::byte, ScratchAllocator> _scratch;

    block::FusedTagList _tagsA;
    block::FusedTagList _tagsB;
    const void*         _chunkInput   = nullptr;
    std::size_t         _activeFirst  = 0UZ;
    std::size_t         _activeLast   = 0UZ;
    bool                _ratioLatched = false;

    explicit FusedRun(const RunPlan& plan, std::shared_ptr<gr::Sequence> progress) : _members(plan.members), _stages(plan.stages), _progress(std::move(progress)) {
        msgIn  = nullptr; // a run is never a graph member, so it is never wired into the message plane
        msgOut = nullptr;

        _chunkSamples     = plan.chunkSamples;
        _quantum          = plan.quantum;
        _outputPerQuantum = plan.outputPerQuantum;

        _fusedStages.reserve(_members.size());
        std::size_t maxValueSize   = 0UZ;
        std::size_t maxSegmentSize = 0UZ;
        _inPlaceScratch            = true;
        for (const std::shared_ptr<BlockModel>& member : _members) {
            _fusedStages.push_back(member->fusedStage());
        }
        for (const Stage& stage : _stages) {
            if (!stage.isComposed) {
                continue;
            }
            maxSegmentSize = std::max(maxSegmentSize, stage.inputSamples);
            for (std::size_t k = 0UZ; k < stage.nMembers; ++k) {
                const block::FusedStage* fused = _fusedStages[stage.firstMember + k];
                maxValueSize                   = std::max({maxValueSize, fused->valueSizeIn, fused->valueSizeOut});
                _inPlaceScratch                = _inPlaceScratch && fused->valueSizeIn == fused->valueSizeOut;
            }
        }

        _scratchStride = maxSegmentSize * maxValueSize;
        _scratch.resize((_inPlaceScratch ? 1UZ : 2UZ) * _scratchStride);

        _name       = std::format("fused[{}..{}]", _members.front()->name(), _members.back()->name());
        _uniqueName = std::format("fused[{}..{}]", _members.front()->uniqueName(), _members.back()->uniqueName());

        _dynamicPortsLoader.fn       = &FusedRun::loadBoundaryPorts;
        _dynamicPortsLoader.instance = this;
    }

    [[nodiscard]] const RunMembers&         members() const noexcept { return _members; }
    [[nodiscard]] const std::vector<Stage>& stages() const noexcept { return _stages; }
    [[nodiscard]] std::size_t               chunkSize() const noexcept { return _chunkSamples; }

    [[nodiscard]] work::Result work(std::size_t requestedWork) override {
        using enum work::Status;
        if (_ratioLatched || ratioChanged()) { // the quantum and the interior rings were derived from the planned ratios
            _ratioLatched = true;
            return workMembersIndividually(requestedWork);
        }

        std::size_t  performed = 0UZ;
        work::Status merged    = DONE;
        for (std::size_t stageIndex = 0UZ; stageIndex < _stages.size(); ++stageIndex) {
            const Stage& stage = _stages[stageIndex];
            work::Status status;
            if (stage.isComposed) {
                const SegmentResult segment = driveSegment(stageIndex, requestedWork);
                if (segment.fallback) { // end of stream, or a member that is no longer RUNNING: each member shuts itself down
                    return workMembersIndividually(requestedWork);
                }
                performed += segment.produced;
                status = segment.status;
            } else { // a short stage is not an error: the surplus stays in the interior ring and the next pass takes it
                const auto [_, memberWork, memberStatus] = _members[stage.firstMember]->work(stage.inputSamples);
                performed += memberWork;
                status = memberStatus;
            }
            if (status == ERROR) {
                return {requestedWork, performed, ERROR};
            }
            merged = merged == DONE ? status : (status == DONE ? merged : OK);
        }

        if (performed > 0UZ) {
            _progress->incrementAndGet();
            _progress->notify_all();
        }
        return {requestedWork, performed, merged};
    }

    void processScheduledMessages() override {
        for (const std::shared_ptr<BlockModel>& member : _members) {
            member->processScheduledMessages();
        }
    }

    void init(std::shared_ptr<gr::Sequence> progress, std::string_view /*ioThreadPool*/) override { _progress = std::move(progress); }

    [[nodiscard]] std::expected<void, Error> changeStateTo(lifecycle::State newState) noexcept override {
        std::expected<void, Error> result;
        for (const std::shared_ptr<BlockModel>& member : _members) {
            if (auto memberResult = member->changeStateTo(newState); !memberResult.has_value() && result.has_value()) {
                result = std::move(memberResult);
            }
        }
        return result;
    }

    [[nodiscard]] lifecycle::State state() const noexcept override {
        lifecycle::State result = lifecycle::State::ERROR;
        for (const std::shared_ptr<BlockModel>& member : _members) {
            const lifecycle::State memberState = member->state();
            if (memberState == lifecycle::State::ERROR) {
                return lifecycle::State::ERROR;
            }
            result = std::min(result, memberState);
        }
        return result;
    }

    [[nodiscard]] constexpr bool isBlocking() const noexcept override {
        return std::ranges::any_of(_members, [](const std::shared_ptr<BlockModel>& member) { return member->isBlocking(); });
    }

    [[nodiscard]] std::string_view name() const override { return _name; }
    [[nodiscard]] std::string_view uniqueName() const override { return _uniqueName; }
    [[nodiscard]] std::string_view typeName() const override { return _typeName; }
    void                           setName(std::string name) noexcept override { _name = std::move(name); }

    [[nodiscard]] property_map&       metaInformation() noexcept override { return _members.front()->metaInformation(); }
    [[nodiscard]] const property_map& metaInformation() const override { return _members.front()->metaInformation(); }
    [[nodiscard]] property_map&       uiConstraints() noexcept override { return _members.front()->uiConstraints(); }
    [[nodiscard]] const property_map& uiConstraints() const override { return _members.front()->uiConstraints(); }

    // a run is not a settings target; messages reach the members by unique_name through processScheduledMessages()
    [[nodiscard]] SettingsBase&       settings() override { return _members.front()->settings(); }
    [[nodiscard]] const SettingsBase& settings() const override { return _members.front()->settings(); }

    [[nodiscard]] work::Status draw(const property_map& /*config*/) override { return work::Status::ERROR; }

    [[nodiscard]] gr::Ratio  resamplingRatio() const noexcept override { return {static_cast<std::int32_t>(_quantum), static_cast<std::int32_t>(_outputPerQuantum)}; }
    [[nodiscard]] gr::Size_t stride() const noexcept override { return 0U; }

    [[nodiscard]] std::span<const port::BitMask> blockInputTypes() noexcept override { return _members.front()->blockInputTypes(); }
    [[nodiscard]] std::span<const port::BitMask> blockOutputTypes() noexcept override { return _members.back()->blockOutputTypes(); }
    [[nodiscard]] std::span<const std::size_t>   availableInputSamples(bool reset = false) noexcept override { return _members.front()->availableInputSamples(reset); }
    [[nodiscard]] std::span<const std::size_t>   availableOutputSamples(bool reset = false) noexcept override { return _members.back()->availableOutputSamples(reset); }
    [[nodiscard]] std::span<const std::size_t>   minInputRequirements() noexcept override { return _members.front()->minInputRequirements(); }
    [[nodiscard]] std::span<const std::size_t>   maxInputRequirements() noexcept override { return _members.front()->maxInputRequirements(); }
    [[nodiscard]] std::span<const std::size_t>   minOutputRequirements() noexcept override { return _members.back()->minOutputRequirements(); }
    [[nodiscard]] std::span<const std::size_t>   maxOutputRequirements() noexcept override { return _members.back()->maxOutputRequirements(); }
    [[nodiscard]] bool                           hasAsyncInputPorts() noexcept override { return _members.front()->hasAsyncInputPorts(); }
    [[nodiscard]] bool                           hasAsyncOutputPorts() noexcept override { return _members.back()->hasAsyncOutputPorts(); }
    [[nodiscard]] std::vector<gr::PortMetaInfo>  inputMetaInfos(bool reset = true) noexcept override { return _members.front()->inputMetaInfos(reset); }
    [[nodiscard]] std::vector<gr::PortMetaInfo>  outputMetaInfos(bool reset = true) noexcept override { return _members.back()->outputMetaInfos(reset); }

    [[nodiscard]] std::expected<std::size_t, gr::Error> primeInputPort(std::size_t portIdx, std::size_t nSamples, std::source_location loc = std::source_location::current()) noexcept override { return _members.front()->primeInputPort(portIdx, nSamples, loc); }

    [[nodiscard]] std::span<std::shared_ptr<BlockModel>>       blocks() noexcept override { return {}; }
    [[nodiscard]] std::span<const std::shared_ptr<BlockModel>> blocks() const noexcept override { return {}; }
    [[nodiscard]] std::span<Edge>                              edges() noexcept override { return {}; }
    [[nodiscard]] std::span<const Edge>                        edges() const noexcept override { return {}; }

    [[nodiscard]] void*                      raw() override { return nullptr; }
    [[nodiscard]] gr::Graph*                 graph() override { return nullptr; }
    [[nodiscard]] gr::property_map           exportedInputPorts() override { return {}; }
    [[nodiscard]] gr::property_map           exportedOutputPorts() override { return {}; }
    [[nodiscard]] std::expected<void, Error> exportPort(bool, std::string_view, PortDirection, std::string_view, std::string_view, std::source_location location = std::source_location::current()) override { return std::unexpected(Error("a fused run does not export ports", location)); }

    [[nodiscard]] bool isRatioLatched() const noexcept { return _ratioLatched; }

protected:
    // one ratio read per bulk stage, off the sample path; a ratio the run did not plan for cannot be served by rings it cannot resize
    [[nodiscard]] bool ratioChanged() const noexcept {
        return std::ranges::any_of(_stages, [this](const Stage& stage) {
            if (stage.isComposed) {
                return false;
            }
            const gr::Ratio live = _members[stage.firstMember]->resamplingRatio();
            return live.numerator != stage.ratio.numerator || live.denominator != stage.ratio.denominator;
        });
    }

    struct SegmentResult {
        std::size_t  produced = 0UZ;
        work::Status status   = work::Status::OK;
        bool         fallback = false;
    };

    [[nodiscard]] SegmentResult driveSegment(std::size_t stageIndex, std::size_t requestedWork) {
        using enum work::Status;
        const Stage&      stage = _stages[stageIndex];
        const std::size_t first = stage.firstMember;
        const std::size_t last  = first + stage.nMembers - 1UZ;

        const block::FusedFront front = _fusedStages[first]->front(_members[first]->raw(), requestedWork);
        if (front.fallback) {
            return {0UZ, OK, true};
        }
        if (front.nSamples == 0UZ) {
            return {0UZ, front.status, false};
        }

        const std::span<const std::size_t> availableOut = _members[last]->availableOutputSamples(true);
        const std::span<const std::size_t> maxOut       = _members[last]->maxOutputRequirements();
        const std::span<const std::size_t> minOut       = _members[last]->minOutputRequirements();
        std::size_t                        nSamples     = std::min({front.nSamples, stage.inputSamples, availableOut.empty() ? 0UZ : availableOut[0]});
        if (!maxOut.empty()) {
            nSamples = std::min(nSamples, maxOut[0]);
        }
        if (nSamples == 0UZ || (!minOut.empty() && nSamples < minOut[0])) {
            return {0UZ, INSUFFICIENT_OUTPUT_ITEMS, false};
        }

        _activeFirst = first;
        _activeLast  = last;
        return {_fusedStages[first]->withInput(_members[first]->raw(), nSamples, &runChunkThunk, this), OK, false};
    }

    [[nodiscard]] work::Result workMembersIndividually(std::size_t requestedWork) {
        std::size_t performed  = 0UZ;
        bool        unfinished = false;
        for (const std::shared_ptr<BlockModel>& member : _members) {
            const auto [_, memberWork, status] = member->work(requestedWork);
            performed += memberWork;
            if (status == work::Status::ERROR) {
                return {requestedWork, performed, work::Status::ERROR};
            }
            unfinished = unfinished || status != work::Status::DONE;
        }
        return {requestedWork, performed, unfinished ? work::Status::OK : work::Status::DONE};
    }

private:
    static void loadBoundaryPorts(BlockModel* base) {
        auto* self = static_cast<FusedRun*>(base);
        if (const auto port = self->_members.front()->dynamicInputPort(0UZ); port.has_value()) {
            self->_dynamicInputPorts.push_back(port.value()->weakRef());
        }
        if (const auto port = self->_members.back()->dynamicOutputPort(0UZ); port.has_value()) {
            self->_dynamicOutputPorts.push_back(port.value()->weakRef());
        }
    }

    static std::size_t runChunkThunk(void* context, const void* input, const block::FusedTagList& inputTags, std::size_t nSamples) { return static_cast<FusedRun*>(context)->runChunk(input, inputTags, nSamples); }
    static std::size_t runPassesThunk(void* context, void* output, std::size_t nSamples) { return static_cast<FusedRun*>(context)->runPasses(output, nSamples); }

    std::size_t runChunk(const void* input, const block::FusedTagList& inputTags, std::size_t nSamples) {
        const block::FusedTagList* incoming    = std::addressof(inputTags);
        block::FusedTagList*       pingPong[2] = {std::addressof(_tagsA), std::addressof(_tagsB)};
        for (std::size_t k = _activeFirst; k <= _activeLast; ++k) {
            block::FusedTagList& outgoing = *pingPong[(k - _activeFirst) % 2UZ];
            _fusedStages[k]->beginChunk(_members[k]->raw(), k == _activeFirst, *incoming, outgoing);
            incoming = std::addressof(outgoing);
        }
        _chunkInput = input;
        return _fusedStages[_activeLast]->withOutput(_members[_activeLast]->raw(), nSamples, *incoming, &runPassesThunk, this);
    }

    std::size_t runPasses(void* output, std::size_t nSamples) {
        const void* source   = _chunkInput;
        std::size_t produced = nSamples;
        for (std::size_t k = _activeFirst; k < _activeLast; ++k) {
            void* target = _scratch.data() + (_inPlaceScratch ? 0UZ : ((k - _activeFirst) % 2UZ) * _scratchStride);
            produced     = _fusedStages[k]->applyChunk(_members[k]->raw(), source, target, produced);
            source       = target;
        }
        return _fusedStages[_activeLast]->applyChunk(_members[_activeLast]->raw(), source, output, produced);
    }
};

} // namespace gr::fusion

#endif // GNURADIO_FUSED_RUN_HPP
