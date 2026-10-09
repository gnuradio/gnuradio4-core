#ifndef GNURADIO_SEGMENTEDBUFFER_HPP
#define GNURADIO_SEGMENTEDBUFFER_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <compare>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <print>
#include <utility>

#include "Buffer.hpp"
#include "CircularBuffer.hpp"
#include "ClaimStrategy.hpp"
#include "Sequence.hpp"
#include "WaitStrategy.hpp"

namespace gr {

/**
 * @brief Single-producer buffer whose slots live in fixed-size segments. The writer allocates a segment when it first
 * writes a slot in it, and frees the segment once every reader has passed it.
 *
 * The writer and each reader keep absolute positions, as in CircularBuffer, and the claim holds the writer at most
 * size() slots ahead of the slowest reader it has seen. A reservation claims positions and allocates nothing. A
 * segment index maps a position to its slot, so the reader and writer spans are random-access ranges. A span is not
 * contiguous: it may cross from one segment into the next. A buffer whose writer writes no slot allocates no segment
 * and no index.
 *
 * The index starts with kInitialIndexLength entries and doubles when the segments in use outnumber its entries. While
 * no reader attaches, the claim keeps it at most at the next power of two above the number of segments that a window
 * of size() slots can touch. A reader span keeps the index it was created with. The writer frees a replaced index
 * once every reader has consumed past the position it had published when it replaced that index. When it next
 * reserves, the writer frees the segments that lie wholly below both the slowest reader and the publish position.
 *
 * A reader that attaches starts at the publish position it reads while it registers, as a CircularBuffer reader does.
 * The reader marks itself as attaching before it reads that position, and clears the mark once it is in the reader
 * set. The writer frees nothing while a reader is marked. A freeing round checks for the mark after a fence. A round
 * that finds no mark either sees the new reader in the reader set or precedes the reader's read of the publish
 * position. Either way, the round frees only below that position. The writer can run more than size() slots past a
 * reader that it has not yet seen. Once it sees the reader, its claim refuses slots until the reader comes within
 * size() slots, and a round frees only below the reader.
 */
template<typename T, std::size_t kSegmentSize = 32UZ, WaitStrategyLike TWaitStrategy = SleepingWaitStrategy>
class SegmentedBuffer {
    static_assert(std::has_single_bit(kSegmentSize), "the segment size is a power of two");

public:
    using value_type = T;
    using Allocator  = std::pmr::polymorphic_allocator<T>;
    using BufferType = SegmentedBuffer<T, kSegmentSize, TWaitStrategy>;
    using ClaimType  = SingleProducerStrategy<std::dynamic_extent, TWaitStrategy>;

    static constexpr std::size_t kSlotsPerSegment    = kSegmentSize;
    static constexpr std::size_t kInitialIndexLength = 4UZ;
    static constexpr std::size_t kSegmentAlignment   = 64UZ; // a segment starts on a cache line of its own

private:
    static constexpr std::size_t kSegmentShift = static_cast<std::size_t>(std::countr_zero(kSegmentSize));
    static constexpr std::size_t kSlotMask     = kSegmentSize - 1UZ;

    struct alignas(kSegmentAlignment) Segment {
        std::array<T, kSegmentSize> slots{};
    };

    using SegmentEntry = std::atomic<Segment*>;

    // Entry n & (length - 1) holds segment n. The entries of a replaced index stay as they were when it was replaced.
    struct SegmentIndex {
        std::size_t   length;
        SegmentEntry* entries;
        SegmentIndex* replaced{nullptr}; // the index this one replaced, until the writer frees it
        std::size_t   replacedAt{0UZ};   // the publish position at which this index replaced the previous one

        [[nodiscard]] SegmentEntry& entry(std::size_t segmentNumber) const noexcept { return entries[segmentNumber & (length - 1UZ)]; }
        [[nodiscard]] Segment*      segment(std::size_t segmentNumber) const noexcept { return entry(segmentNumber).load(std::memory_order_relaxed); }
        [[nodiscard]] const T&      slot(std::size_t position) const noexcept { return segment(position >> kSegmentShift)->slots[position & kSlotMask]; }
    };

    struct BufferImpl {
        Allocator         _allocator;
        const std::size_t _size;
        ClaimType         _claimStrategy;
        std::size_t       _reader_count{0UZ};
        std::size_t       _writer_count{0UZ};

        std::atomic<std::size_t> _nAttachingReaders{0UZ}; // readers that have marked themselves as attaching

        // A reader loads _index after the publish cursor, so it finds the entry of every position below that cursor.
        // The writer alone writes the index, its entries and the members below.
        std::atomic<SegmentIndex*> _index{nullptr};
        std::size_t                _nextSegment{0UZ};   // the segments below this number have been allocated
        std::size_t                _oldestSegment{0UZ}; // the segments below this number have been freed

        BufferImpl(std::size_t minSize, Allocator allocator) : _allocator(allocator), _size(minSize), _claimStrategy(ClaimType(minSize)) {}

        BufferImpl(const BufferImpl&)            = delete;
        BufferImpl(BufferImpl&&)                 = delete;
        BufferImpl& operator=(const BufferImpl&) = delete;
        BufferImpl& operator=(BufferImpl&&)      = delete;

        ~BufferImpl() {
            SegmentIndex* index = _index.load(std::memory_order_relaxed);
            if (index == nullptr) {
                return;
            }
            for (std::size_t segment = _oldestSegment; segment < _nextSegment; ++segment) {
                freeSegment(*index, segment);
            }
            while (index != nullptr) {
                index = freeIndex(index);
            }
        }

        [[nodiscard]] SegmentIndex* writerIndex() const noexcept { return _index.load(std::memory_order_relaxed); }

        [[nodiscard]] T& writableSlot(std::size_t position) {
            allocateThrough(position + 1UZ);
            return writerIndex()->segment(position >> kSegmentShift)->slots[position & kSlotMask];
        }

        // allocates every segment that holds a position below endPosition
        void allocateThrough(std::size_t endPosition) {
            if (endPosition == 0UZ || ((endPosition - 1UZ) >> kSegmentShift) < _nextSegment) [[likely]] {
                return;
            }
            const std::size_t lastSegment = (endPosition - 1UZ) >> kSegmentShift;
            for (; _nextSegment <= lastSegment; ++_nextSegment) {
                SegmentIndex* index = writerIndex();
                if (index == nullptr) {
                    index = replaceIndex(nullptr, kInitialIndexLength);
                } else if (_nextSegment - _oldestSegment >= index->length) {
                    releasePassedSegments();
                    if (_nextSegment - _oldestSegment >= index->length) {
                        index = replaceIndex(index, 2UZ * index->length);
                    }
                }
                std::pmr::polymorphic_allocator<Segment> segmentAllocator(_allocator);
                Segment*                                 segment = segmentAllocator.allocate(1UZ);
                segmentAllocator.construct(segment);
                index->entry(_nextSegment).store(segment, std::memory_order_relaxed);
            }
        }

        // the lowest position a reader in the reader set may still read. The claim keeps the writer within size()
        // slots of each reader in its snapshot, and a remaining capacity above zero gives the slowest of them. A reader
        // that joined the set before the snapshot held it can lie more than size() slots behind the writer. At zero
        // capacity, the writer reads the position of each reader in the set.
        [[nodiscard]] std::size_t slowestReaderPosition() const noexcept {
            const std::size_t remaining = _claimStrategy.getRemainingCapacity();
            if (remaining > 0UZ) [[likely]] {
                return _claimStrategy._reserveCursor - (_size - remaining);
            }
            const auto  readers = gr::detail::loadSequences(_claimStrategy._readSequences);
            std::size_t slowest = _claimStrategy._reserveCursor;
            for (const auto& reader : *readers) {
                slowest = std::min(slowest, reader->value());
            }
            return slowest;
        }

        // frees the segments that lie wholly below the slowest reader and the publish position, and the replaced
        // indexes no reader can hold
        void releasePassedSegments() noexcept {
            SegmentIndex* index = writerIndex();
            if (index == nullptr) [[likely]] {
                return;
            }
            const std::size_t published          = _claimStrategy._publishCursor.value();
            const bool        segmentMayBePassed = _oldestSegment < _nextSegment && ((_oldestSegment + 1UZ) << kSegmentShift) <= published;
            const bool        replacedMayBeFreed = index->replaced != nullptr && published > index->replacedAt;
            if (!segmentMayBePassed && !replacedMayBeFreed) {
                return;
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (_nAttachingReaders.load(std::memory_order_acquire) != 0UZ) {
                return;
            }
            // with no reader, the slowest position is the reserve cursor, which a reservation holds above the publish position
            const std::size_t slowestReader = std::min(slowestReaderPosition(), published);
            for (; _oldestSegment < _nextSegment && ((_oldestSegment + 1UZ) << kSegmentShift) <= slowestReader; ++_oldestSegment) {
                freeSegment(*index, _oldestSegment);
            }
            // A reader advances only when its last span is released. A reader past the position at which the index
            // was replaced has therefore released every span that holds an earlier index.
            if (index->replaced != nullptr && slowestReader > index->replacedAt) {
                SegmentIndex* replaced = std::exchange(index->replaced, nullptr);
                while (replaced != nullptr) {
                    replaced = freeIndex(replaced);
                }
            }
        }

        // installs an index of `length` entries that holds every allocated segment of `current`, and publishes it
        [[nodiscard]] SegmentIndex* replaceIndex(SegmentIndex* current, std::size_t length) {
            std::pmr::polymorphic_allocator<SegmentEntry> entryAllocator(_allocator);
            SegmentEntry*                                 entries = entryAllocator.allocate(length);
            std::uninitialized_value_construct_n(entries, length);
            std::pmr::polymorphic_allocator<SegmentIndex> indexAllocator(_allocator);
            SegmentIndex*                                 index = indexAllocator.allocate(1UZ);
            indexAllocator.construct(index, SegmentIndex{.length = length, .entries = entries, .replaced = current, .replacedAt = _claimStrategy._publishCursor.value()});
            if (current != nullptr) {
                for (std::size_t segment = _oldestSegment; segment < _nextSegment; ++segment) {
                    index->entry(segment).store(current->segment(segment), std::memory_order_relaxed);
                }
            }
            _index.store(index, std::memory_order_release);
            return index;
        }

        // frees `index` and returns the index it replaced
        [[nodiscard]] SegmentIndex* freeIndex(SegmentIndex* index) noexcept {
            SegmentIndex* replaced = index->replaced;
            std::pmr::polymorphic_allocator<SegmentEntry>(_allocator).deallocate(index->entries, index->length);
            std::pmr::polymorphic_allocator<SegmentIndex> indexAllocator(_allocator);
            std::destroy_at(index);
            indexAllocator.deallocate(index, 1UZ);
            return replaced;
        }

        void freeSegment(const SegmentIndex& index, std::size_t segmentNumber) noexcept {
            Segment* segment = index.entry(segmentNumber).exchange(nullptr, std::memory_order_relaxed);
            if (segment != nullptr) {
                std::pmr::polymorphic_allocator<Segment> segmentAllocator(_allocator);
                std::destroy_at(segment);
                segmentAllocator.deallocate(segment, 1UZ);
            }
        }
    };

    // A writer iterator reaches its slots through the buffer, which allocates a segment at its first write. A reader
    // iterator reads through the index its span was created with.
    template<bool kIsWritable>
    class SlotIterator {
        using Source = std::conditional_t<kIsWritable, BufferImpl, const SegmentIndex>;

        Source*     _source{nullptr};
        std::size_t _position{0UZ};

    public:
        using iterator_concept  = std::random_access_iterator_tag;
        using iterator_category = std::random_access_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using reference         = std::conditional_t<kIsWritable, T&, const T&>;
        using pointer           = std::conditional_t<kIsWritable, T*, const T*>;

        constexpr SlotIterator() noexcept = default;
        constexpr SlotIterator(Source* source, std::size_t position) noexcept : _source(source), _position(position) {}

        [[nodiscard]] reference operator*() const {
            if constexpr (kIsWritable) {
                return _source->writableSlot(_position);
            } else {
                return _source->slot(_position);
            }
        }
        [[nodiscard]] pointer   operator->() const { return std::addressof(**this); }
        [[nodiscard]] reference operator[](difference_type n) const { return *(*this + n); }

        SlotIterator& operator++() noexcept {
            ++_position;
            return *this;
        }
        SlotIterator operator++(int) noexcept {
            SlotIterator previous = *this;
            ++_position;
            return previous;
        }
        SlotIterator& operator--() noexcept {
            --_position;
            return *this;
        }
        SlotIterator operator--(int) noexcept {
            SlotIterator previous = *this;
            --_position;
            return previous;
        }
        SlotIterator& operator+=(difference_type n) noexcept {
            _position = static_cast<std::size_t>(static_cast<difference_type>(_position) + n);
            return *this;
        }
        SlotIterator& operator-=(difference_type n) noexcept { return *this += -n; }

        [[nodiscard]] friend SlotIterator    operator+(SlotIterator it, difference_type n) noexcept { return it += n; }
        [[nodiscard]] friend SlotIterator    operator+(difference_type n, SlotIterator it) noexcept { return it += n; }
        [[nodiscard]] friend SlotIterator    operator-(SlotIterator it, difference_type n) noexcept { return it -= n; }
        [[nodiscard]] friend difference_type operator-(const SlotIterator& lhs, const SlotIterator& rhs) noexcept { return static_cast<difference_type>(lhs._position) - static_cast<difference_type>(rhs._position); }
        [[nodiscard]] friend bool            operator==(const SlotIterator& lhs, const SlotIterator& rhs) noexcept { return lhs._position == rhs._position; }
        [[nodiscard]] friend auto            operator<=>(const SlotIterator& lhs, const SlotIterator& rhs) noexcept { return lhs._position <=> rhs._position; }
    };

    template<typename U = T>
    class Writer;

    struct RefusedReservation {};

    template<typename U = T, SpanReleasePolicy policy = SpanReleasePolicy::ProcessNone>
    class WriterSpan {
        Writer<U>* _parent    = nullptr;
        bool       _isRefused = false;

        [[nodiscard]] constexpr std::size_t claimSize() const noexcept { return _isRefused ? 0UZ : _parent->_claimSize; }

    public:
        using element_type = T;
        using value_type   = typename std::remove_cv_t<T>;
        using iterator     = SlotIterator<true>;

        explicit WriterSpan(Writer<U>* parent) noexcept : _parent(parent) { _parent->incInstanceCount(); }
        explicit WriterSpan(Writer<U>* parent, RefusedReservation) noexcept : _parent(parent), _isRefused(true) { _parent->incInstanceCount(); }
        explicit WriterSpan(Writer<U>* parent, std::size_t claimEnd, std::size_t nSlotsToClaim) noexcept : _parent(parent) {
            _parent->_claimOffset = claimEnd - nSlotsToClaim;
            _parent->_claimSize   = nSlotsToClaim;
            _parent->incInstanceCount();
        }
        WriterSpan(const WriterSpan& other) : _parent(other._parent), _isRefused(other._isRefused) { _parent->incInstanceCount(); }
        WriterSpan& operator=(const WriterSpan&) = delete;

        ~WriterSpan() {
            _parent->decInstanceCount();
            if (_isRefused || !_parent->isLastInstance()) {
                return;
            }
            if (!_parent->isPublishRequested()) {
                if constexpr (spanReleasePolicy() == SpanReleasePolicy::Terminate) {
                    assert(false && "SegmentedBuffer::WriterSpan() - omitted publish() call for SpanReleasePolicy::Terminate");
                    std::abort();
                } else if constexpr (spanReleasePolicy() == SpanReleasePolicy::ProcessAll) {
                    publish(_parent->_claimSize);
                } else {
                    publish(0UZ);
                }
            }
            BufferImpl&       impl       = *_parent->_buffer;
            const std::size_t nPublished = _parent->_nRequestedSamplesToPublish;
            impl.allocateThrough(_parent->_claimOffset + nPublished); // a published slot the writer left untouched holds a default value
            impl._claimStrategy.publish(_parent->_claimOffset, nPublished);
            _parent->_claimOffset += nPublished;
            _parent->_nRequestedSamplesToPublish = 0UZ;
            _parent->_claimSize                  = 0UZ;
        }

        [[nodiscard]] constexpr static SpanReleasePolicy spanReleasePolicy() noexcept { return policy; }
        [[nodiscard]] constexpr std::size_t              nRequestedSamplesToPublish() const noexcept { return _parent->nRequestedSamplesToPublish(); }
        [[nodiscard]] constexpr bool                     isPublishRequested() const noexcept { return _parent->isPublishRequested(); }
        [[nodiscard]] constexpr bool                     isFullyPublished() const noexcept { return _parent->_claimSize == _parent->_nRequestedSamplesToPublish; }
        [[nodiscard]] constexpr std::size_t              instanceCount() const noexcept { return _parent->instanceCount(); }

        [[nodiscard]] constexpr std::size_t size() const noexcept { return claimSize(); }
        [[nodiscard]] constexpr bool        empty() const noexcept { return claimSize() == 0UZ; }
        [[nodiscard]] iterator              begin() const noexcept { return iterator(_parent->_buffer.get(), _parent->_claimOffset); }
        [[nodiscard]] iterator              end() const noexcept { return iterator(_parent->_buffer.get(), _parent->_claimOffset + claimSize()); }
        [[nodiscard]] T&                    operator[](std::size_t i) const { return _parent->_buffer->writableSlot(_parent->_claimOffset + i); }

        // publishing fewer slots than reserved releases the remainder; an over-publish is clamped and reported
        constexpr void publish(std::size_t nSlotsToPublish) noexcept {
            if (_isRefused) {
                return;
            }
            auto&             requested        = _parent->_nRequestedSamplesToPublish;
            const std::size_t alreadyRequested = (requested == Writer<U>::kNotPublished) ? 0UZ : requested;
            const std::size_t unpublished      = _parent->_claimSize - alreadyRequested;
            if (nSlotsToPublish > unpublished) [[unlikely]] {
                std::print(stderr, "SegmentedBuffer::WriterSpan::publish({}) exceeds the {} unpublished slots of a {}-slot reservation - clamped\n", nSlotsToPublish, unpublished, _parent->_claimSize);
                nSlotsToPublish = unpublished;
            }
            requested = alreadyRequested + nSlotsToPublish;
        }
    };

    template<typename U>
    class Writer {
        friend class WriterSpan<U, SpanReleasePolicy::Terminate>;
        friend class WriterSpan<U, SpanReleasePolicy::ProcessAll>;
        friend class WriterSpan<U, SpanReleasePolicy::ProcessNone>;

        static constexpr std::size_t kNotPublished = std::numeric_limits<std::size_t>::max();

        std::shared_ptr<BufferImpl> _buffer;
        std::size_t                 _nRequestedSamplesToPublish{0UZ};
        std::size_t                 _claimOffset{0UZ};
        std::size_t                 _claimSize{0UZ};
        std::size_t                 _instanceCount{0UZ};

        constexpr void                      incInstanceCount() noexcept { _instanceCount++; }
        constexpr void                      decInstanceCount() noexcept { _instanceCount--; }
        [[nodiscard]] constexpr bool        isLastInstance() const noexcept { return _instanceCount == 0UZ; }
        [[nodiscard]] constexpr std::size_t instanceCount() const noexcept { return _instanceCount; }

        template<SpanReleasePolicy policy>
        [[nodiscard]] WriterSpan<U, policy> claim(std::size_t nSlots, bool mayWait) noexcept {
            if (_instanceCount > 0UZ) [[unlikely]] {
                std::print(stderr, "SegmentedBuffer::Writer::reserve({}) nested inside a live {}-slot reservation - refused\n", nSlots, _claimSize);
                return WriterSpan<U, policy>(this, RefusedReservation{});
            }
            _nRequestedSamplesToPublish = kNotPublished;
            _buffer->releasePassedSegments();
            if (nSlots == 0UZ) {
                return WriterSpan<U, policy>(this);
            }
            if (mayWait) {
                return WriterSpan<U, policy>(this, _buffer->_claimStrategy.next(nSlots), nSlots);
            }
            const std::optional<std::size_t> claimEnd = _buffer->_claimStrategy.tryNext(nSlots);
            if (!claimEnd.has_value()) {
                return WriterSpan<U, policy>(this);
            }
            return WriterSpan<U, policy>(this, *claimEnd, nSlots);
        }

    public:
        Writer() = delete;
        explicit Writer(std::shared_ptr<BufferImpl> buffer) noexcept : _buffer(std::move(buffer)) {
            gr::atomic_ref(_buffer->_writer_count).fetch_add(1UZ);
            _claimOffset = _buffer->_claimStrategy._publishCursor.value();
        }
        Writer(Writer&& other) noexcept : _buffer(std::move(other._buffer)), _nRequestedSamplesToPublish(std::exchange(other._nRequestedSamplesToPublish, 0UZ)), _claimOffset(std::exchange(other._claimOffset, 0UZ)), _claimSize(std::exchange(other._claimSize, 0UZ)) {}
        Writer(const Writer&)            = delete;
        Writer& operator=(const Writer&) = delete;
        Writer& operator=(Writer&& other) noexcept {
            std::swap(_buffer, other._buffer);
            std::swap(_nRequestedSamplesToPublish, other._nRequestedSamplesToPublish);
            std::swap(_claimOffset, other._claimOffset);
            std::swap(_claimSize, other._claimSize);
            return *this;
        }
        ~Writer() {
            if (_buffer) {
                gr::atomic_ref(_buffer->_writer_count).fetch_sub(1UZ);
            }
        }

        [[nodiscard]] BufferType  buffer() const noexcept { return SegmentedBuffer(_buffer); }
        [[nodiscard]] std::size_t nReaders() const noexcept { return gr::atomic_ref(_buffer->_reader_count).load_acquire(); }
        [[nodiscard]] std::size_t nWriters() const noexcept { return gr::atomic_ref(_buffer->_writer_count).load_acquire(); }

        template<SpanReleasePolicy policy = SpanReleasePolicy::ProcessNone>
        [[nodiscard]] auto tryReserve(std::size_t nSlots) noexcept -> WriterSpan<U, policy> {
            return claim<policy>(nSlots, false);
        }

        template<SpanReleasePolicy policy = SpanReleasePolicy::ProcessNone>
        [[nodiscard]] auto reserve(std::size_t nSlots) noexcept -> WriterSpan<U, policy> {
            return claim<policy>(nSlots, true);
        }

        [[nodiscard]] std::size_t                position() const noexcept { return _buffer->_claimStrategy._publishCursor.value(); }
        [[nodiscard]] std::size_t                available() const noexcept { return _buffer->_claimStrategy.getRemainingCapacity(); }
        [[nodiscard]] std::pmr::memory_resource* resource() const noexcept { return _buffer->_allocator.resource(); }
        [[nodiscard]] constexpr bool             isPublishRequested() const noexcept { return _nRequestedSamplesToPublish != kNotPublished; }
        [[nodiscard]] constexpr std::size_t      nRequestedSamplesToPublish() const noexcept { return _nRequestedSamplesToPublish == kNotPublished ? 0UZ : _nRequestedSamplesToPublish; }
    };

    template<typename U = T>
    class Reader;

    template<typename U = T, SpanReleasePolicy policy = SpanReleasePolicy::ProcessNone>
    class ReaderSpan {
        Reader<U>*          _parent = nullptr;
        const SegmentIndex* _index  = nullptr;
        std::size_t         _first{0UZ};
        std::size_t         _size{0UZ};

    public:
        using element_type = T;
        using value_type   = typename std::remove_cv_t<T>;
        using iterator     = SlotIterator<false>;

        explicit ReaderSpan(Reader<U>* parent, std::size_t first, std::size_t nRequested) noexcept : _parent(parent), _index(parent->_buffer->_index.load(std::memory_order_acquire)), _first(first), _size(nRequested) { _parent->incInstanceCount(); }
        ReaderSpan(const ReaderSpan& other) : _parent(other._parent), _index(other._index), _first(other._first), _size(other._size) { _parent->incInstanceCount(); }
        ReaderSpan& operator=(const ReaderSpan&) = delete;

        ~ReaderSpan() {
            _parent->decInstanceCount();
            if (!_parent->isLastInstance()) {
                return;
            }
            if (_parent->isConsumeRequested()) {
                std::ignore = performConsume<false>(_parent->_nRequestedSamplesToConsume);
            } else if constexpr (spanReleasePolicy() == SpanReleasePolicy::Terminate) {
                assert(false && "SegmentedBuffer::ReaderSpan() - omitted consume() call for SpanReleasePolicy::Terminate");
                std::abort();
            } else if constexpr (spanReleasePolicy() == SpanReleasePolicy::ProcessAll) {
                std::ignore = performConsume(_parent->_nSamplesFirstGet);
            } else {
                std::ignore = performConsume(0UZ);
            }
        }

        [[nodiscard]] constexpr static SpanReleasePolicy spanReleasePolicy() noexcept { return policy; }
        [[nodiscard]] constexpr bool                     isConsumeRequested() const noexcept { return _parent->isConsumeRequested(); }
        [[nodiscard]] constexpr std::size_t              instanceCount() const noexcept { return _parent->instanceCount(); }
        [[nodiscard]] constexpr std::size_t              nRequestedSamplesToConsume() const { return _parent->nRequestedSamplesToConsume(); }

        [[nodiscard]] constexpr std::size_t size() const noexcept { return _size; }
        [[nodiscard]] constexpr bool        empty() const noexcept { return _size == 0UZ; }
        [[nodiscard]] iterator              begin() const noexcept { return iterator(_index, _first); }
        [[nodiscard]] iterator              end() const noexcept { return iterator(_index, _first + _size); }
        [[nodiscard]] const T&              front() const noexcept { return (*this)[0UZ]; }
        [[nodiscard]] const T&              back() const noexcept { return (*this)[_size - 1UZ]; }
        [[nodiscard]] const T&              operator[](std::size_t i) const noexcept { return _index->slot(_first + i); }

        template<bool strict_check = true>
        [[nodiscard]] bool consume(std::size_t nSamples) noexcept {
            assert(!isConsumeRequested() && "SegmentedBuffer::ReaderSpan::consume() called a second time for one span");
            return tryConsume<strict_check>(nSamples);
        }

        template<bool strict_check = true>
        [[nodiscard]] bool tryConsume(std::size_t nSamples) noexcept {
            if (isConsumeRequested()) {
                return false;
            }
            if constexpr (strict_check) {
                const std::size_t nSamplesHandedOut = _parent->_nSamplesFirstGet == std::numeric_limits<std::size_t>::max() ? 0UZ : _parent->_nSamplesFirstGet;
                if (nSamples > nSamplesHandedOut) {
                    std::print(stderr, "SegmentedBuffer::ReaderSpan::consume({}) exceeds the {}-slot span handed out by get() - refused\n", nSamples, nSamplesHandedOut);
                    return false;
                }
            }
            _parent->_nRequestedSamplesToConsume = nSamples;
            return true;
        }

    private:
        template<bool strict_check = true>
        [[nodiscard]] bool performConsume(std::size_t nSamples) noexcept {
            _parent->_nSamplesFirstGet           = std::numeric_limits<std::size_t>::max();
            _parent->_nRequestedSamplesToConsume = std::numeric_limits<std::size_t>::max();
            if constexpr (strict_check) {
                if (nSamples == 0UZ) {
                    return true;
                }
                if (nSamples > _parent->available()) {
                    return false;
                }
            }
            const std::size_t nextReadIndex = gr::atomic_ref(_parent->_readIndexCached).load_relaxed() + nSamples;
            gr::atomic_ref(_parent->_readIndexCached).store_relaxed(nextReadIndex);
            _parent->_readIndex->setValue(nextReadIndex);
            _parent->_nSamplesConsumed = nSamples;
            return true;
        }
    };

    template<typename U>
    class Reader {
        friend class ReaderSpan<U, SpanReleasePolicy::Terminate>;
        friend class ReaderSpan<U, SpanReleasePolicy::ProcessAll>;
        friend class ReaderSpan<U, SpanReleasePolicy::ProcessNone>;

        std::shared_ptr<Sequence>   _readIndex = std::make_shared<Sequence>();
        mutable std::size_t         _readIndexCached;
        std::shared_ptr<BufferImpl> _buffer;
        std::size_t                 _nSamplesFirstGet{std::numeric_limits<std::size_t>::max()};
        mutable std::size_t         _instanceCount{0UZ};
        std::size_t                 _nRequestedSamplesToConsume{std::numeric_limits<std::size_t>::max()};
        std::size_t                 _nSamplesConsumed{0UZ};

        constexpr void                      incInstanceCount() const noexcept { _instanceCount++; }
        constexpr void                      decInstanceCount() const noexcept { _instanceCount--; }
        [[nodiscard]] constexpr bool        isLastInstance() const noexcept { return _instanceCount == 0UZ; }
        [[nodiscard]] constexpr std::size_t instanceCount() const noexcept { return _instanceCount; }

    public:
        Reader() = delete;
        explicit Reader(std::shared_ptr<BufferImpl> buffer) noexcept : _buffer(std::move(buffer)) {
            _buffer->_nAttachingReaders.fetch_add(1UZ, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst); // orders the mark before the read of the publish position
            gr::detail::addSequences(_buffer->_claimStrategy._readSequences, _buffer->_claimStrategy._publishCursor, {_readIndex});
            _buffer->_claimStrategy.notifyReaderSetChanged();
            _buffer->_nAttachingReaders.fetch_sub(1UZ, std::memory_order_release);
            gr::atomic_ref(_buffer->_reader_count).fetch_add(1UZ);
            gr::atomic_ref(_readIndexCached).store_relaxed(_readIndex->value());
        }
        Reader(Reader&& other) noexcept : _readIndex(std::move(other._readIndex)), _readIndexCached(std::exchange(other._readIndexCached, _readIndex->value())), _buffer(std::move(other._buffer)), _nSamplesFirstGet(other._nSamplesFirstGet), _instanceCount(other._instanceCount), _nRequestedSamplesToConsume(other._nRequestedSamplesToConsume), _nSamplesConsumed(other._nSamplesConsumed) {}
        Reader(const Reader&)            = delete;
        Reader& operator=(const Reader&) = delete;
        Reader& operator=(Reader&& other) noexcept {
            std::swap(_readIndex, other._readIndex);
            std::swap(_readIndexCached, other._readIndexCached);
            std::swap(_buffer, other._buffer);
            std::swap(_nSamplesFirstGet, other._nSamplesFirstGet);
            std::swap(_instanceCount, other._instanceCount);
            std::swap(_nRequestedSamplesToConsume, other._nRequestedSamplesToConsume);
            std::swap(_nSamplesConsumed, other._nSamplesConsumed);
            return *this;
        }
        ~Reader() {
            if (_buffer) {
                gr::detail::removeSequence(_buffer->_claimStrategy._readSequences, _readIndex);
                _buffer->_claimStrategy.notifyReaderSetChanged();
                gr::atomic_ref(_buffer->_reader_count).fetch_sub(1UZ);
            }
        }

        [[nodiscard]] BufferType  buffer() const noexcept { return SegmentedBuffer(_buffer); }
        [[nodiscard]] std::size_t nReaders() const noexcept { return gr::atomic_ref(_buffer->_reader_count).load_acquire(); }
        [[nodiscard]] std::size_t nWriters() const noexcept { return gr::atomic_ref(_buffer->_writer_count).load_acquire(); }

        [[nodiscard]] constexpr std::size_t nSamplesConsumed() const noexcept { return _nSamplesConsumed; }
        [[nodiscard]] constexpr bool        isConsumeRequested() const noexcept { return _nRequestedSamplesToConsume != std::numeric_limits<std::size_t>::max(); }
        [[nodiscard]] constexpr std::size_t nRequestedSamplesToConsume() const noexcept { return _nRequestedSamplesToConsume; }

        template<SpanReleasePolicy policy = SpanReleasePolicy::ProcessNone>
        [[nodiscard]] auto get(const std::size_t nRequested = std::numeric_limits<std::size_t>::max()) noexcept -> ReaderSpan<U, policy> {
            assert(!isConsumeRequested() && "SegmentedBuffer::Reader::get() called after consume()");
            std::size_t nSlots = nRequested;
            if (nSlots == std::numeric_limits<std::size_t>::max()) {
                nSlots = available();
            } else {
                assert(nSlots <= available() && "more slots requested than are available");
            }
            if (_nSamplesFirstGet == std::numeric_limits<std::size_t>::max()) {
                _nSamplesFirstGet = nSlots;
                _nSamplesConsumed = 0UZ;
            } else {
                nSlots = std::min(nSlots, _nSamplesFirstGet);
            }
            return ReaderSpan<U, policy>(this, position(), nSlots);
        }

        [[nodiscard]] std::size_t position() const noexcept { return gr::atomic_ref(_readIndexCached).load_relaxed(); }
        [[nodiscard]] std::size_t available() const noexcept { return _buffer->_claimStrategy._publishCursor.value() - gr::atomic_ref(_readIndexCached).load_relaxed(); }
    };

    std::shared_ptr<BufferImpl> _sharedBufferPtr;
    explicit SegmentedBuffer(std::shared_ptr<BufferImpl> sharedBufferPtr) : _sharedBufferPtr(std::move(sharedBufferPtr)) {}

public:
    SegmentedBuffer() = delete;
    explicit SegmentedBuffer(std::size_t minSize, Allocator allocator = Allocator()) : _sharedBufferPtr(std::make_shared<BufferImpl>(minSize, allocator)) {}

    [[nodiscard]] std::size_t           size() const noexcept { return _sharedBufferPtr->_size; }
    [[nodiscard]] BufferWriterLike auto new_writer() { return Writer<T>(_sharedBufferPtr); }
    [[nodiscard]] BufferReaderLike auto new_reader() { return Reader<T>(_sharedBufferPtr); }

    [[nodiscard]] std::size_t    n_writers() const { return gr::atomic_ref(_sharedBufferPtr->_writer_count).load_acquire(); }
    [[nodiscard]] std::size_t    n_readers() const { return gr::atomic_ref(_sharedBufferPtr->_reader_count).load_acquire(); }
    [[nodiscard]] constexpr bool isMmapAllocated() const noexcept { return false; }
    [[nodiscard]] const auto&    claim_strategy() { return _sharedBufferPtr->_claimStrategy; }
    [[nodiscard]] const auto&    cursor_sequence() { return _sharedBufferPtr->_claimStrategy._publishCursor; }
};

static_assert(BufferLike<SegmentedBuffer<int>>);

} // namespace gr

#endif // GNURADIO_SEGMENTEDBUFFER_HPP
