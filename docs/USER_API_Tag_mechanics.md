# Tag mechanics — User-API Reference

Tags are key-value metadata attached to specific sample positions in a data stream. They carry
signal descriptions (`gr:sample_rate`, `gr:signal_name`, ...), trigger events and
user-defined metadata. Tags propagate through the graph alongside samples — the framework handles
forwarding, settings synchronisation, and position tracking automatically unless the block opts out.

Tag keys follow the [SigMF](https://sigmf.org/) standard where applicable to simplify import/export
of annotated signal data.

## TL;DR

- **Keys**: a tag key is `domain:name`. Only GNU Radio defines a new `gr:` key. Any block may write
  or read a `gr:` key with the meaning GNU Radio gives it. A block's new keys go in its author's own
  domain. A key with no colon is in the default domain, and `:name` and `name` are one key. The
  constants in `gr::tag::` (`SAMPLE_RATE`, `TRIGGER_NAME`, ...) hold the `gr:` keys.
- **processBulk**: read tags via `input.tags()` on the InputSpan. Publish via
  `outSpan.publishTag(map, offset)`.
- **processOne**: check `inputTagsPresent()`, read via `mergedInputTag()` (returns `const Tag&`, once
  per work call). Publish via `this->publishTag(map)` — the framework places it at the correct sample
  offset.
- **Forwarding**: every block forwards every key of every tag, on the output sample its input sample
  maps to. `DroppedTagKeys<"key", ...>` drops the listed keys. `NoTagPropagation` forwards nothing.
  `BackwardTagPropagation` puts every tag of a chunk on the chunk's first output sample. Override
  `forwardTags()` for full custom control.
- **Settings**: a tag sets a setting only where the application or the graph couples the setting to
  the tag's key: `block.setTagCouplings()` or a graph file's `tag_couplings`. The framework
  writes no tag for a setting.
- Tags are **never merged** across different positions or sources. No implicit data loss.

---

## Tag model

A `Tag` is `{ std::size_t index; property_map map; }` where `index` is the absolute sample position
and `map` is a `std::pmr::unordered_map<std::pmr::string, pmt::Value>`.

Tags live in circular buffers alongside (but separate from) the sample data. Each port has its own
tag buffer. When a block publishes a tag, it goes into the output port's tag buffer. The downstream
block reads it from its input port's tag buffer.

```
  tags:     ▽              ▼                    ▽  ▼        ▽
            │              │                    │  │        │
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─
            │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │  │
           ─┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴─
  index:    0  1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16

  ▽ = signal/settings tag (sample_rate, signal_name, ...)
  ▼ = trigger tag (trigger_name, trigger_time, ...)
```

Multiple tags can exist at the same sample position (e.g., a signal description and a trigger event
at sample 0):

```
  tags:     ▽▼                              two tags at sample 0
            ││
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬─
            │  │  │  │  │  │  │  │
           ─┴──┴──┴──┴──┴──┴──┴──┴─
            0  1  2  3  4  5  6  7
```

Standard tag keys are defined in `gr::tag::` (`SAMPLE_RATE`, `SIGNAL_NAME`, ...) and are `gr:` keys.
Any string key is valid. Every key crosses every block that forwards tags (see "Automatic tag
forwarding" below). A key sets a downstream setting only where that block couples the setting to it
(see the settings section below).

---

## Reading tags

### processBulk

The InputSpan provides direct access to tags as a lazy view — no allocation:

```cpp
work::Status processBulk(InputSpanLike auto& input, OutputSpanLike auto& output) {
    for (const auto& [relIndex, tagMapRef] : input.tags()) {
        // relIndex: position relative to span start (ptrdiff_t, can be negative)
        // tagMapRef: std::reference_wrapper<const property_map>
        const property_map& map = tagMapRef.get();
    }
    return work::Status::OK;
}
```

Use `input.tags(n)` to limit to tags within the first `n` samples. For raw access (manual
consumption): `input.rawTags` is the underlying `ReaderSpan<Tag>`.

A negative `relIndex` is a tag that lies before the current span. The input span retires only the
tags at the chunk's first sample on a block with a `forwardTags()` override, and on a block with
`Stride<>`, an asynchronous stream port or `NoTagPropagation` that declares neither `Resampling<>`
nor `BackwardTagPropagation`. On such a block a tag inside an earlier chunk arrives this way in the
next call (see "Where a forwarded tag lands"). On a block declaring `Stride<>` and supplying
`forwardTags()`, the tags of the samples a stride skipped arrive this way too (see "Custom tag
forwarding — `forwardTags()`").

### processOne

processOne processes one sample per work call when a tag is present. Use the pre-computed merged tag:

```cpp
T processOne(T input) noexcept {
    if (this->inputTagsPresent()) {
        const Tag& tag = this->mergedInputTag();
        // tag.map contains all keys from all sync input ports at relIndex 0
    }
    return input * gain;
}
```

`inputTagsPresent()` is a trivial `bool` check — zero cost per sample. `mergedInputTag()` clears
the flag so subsequent calls in the same work call see stale data. This matches the
one-tag-per-work-call contract for processOne.

`mergedInputTag()` has a `requires` clause — it only compiles for processOne blocks. processBulk
blocks must use `input.tags()`.

---

## Publishing tags

### processBulk

Publish directly on the OutputSpan:

```cpp
work::Status processBulk(InputSpanLike auto& input, OutputSpanLike auto& output) {
    output.publishTag({{"my:event", "peak_detected"}}, 42); // at sample offset 42
    return work::Status::OK;
}
```

A block publishes the keys it defines in its author's own domain, here `my:`. Only GNU Radio
defines a `gr:` key. Any block may publish one, `gr:trigger_name` for instance, with the meaning
GNU Radio gives it.

### processOne

Use `this->publishTag()` — the framework determines the correct sample offset:

```cpp
T processOne(T input) noexcept {
    if (input > threshold) {
        this->publishTag({{"my:event", "threshold_crossed"}});
    }
    return input;
}
```

When processOne publishes a tag, the dispatch loop breaks after the current sample. The next work
call continues from the following sample. This ensures tags are positioned at the exact sample where
`publishTag` was called.

### Publishing while the framework holds the span

Between the moment the framework reserves a work call's output spans and the moment it releases them,
those spans own the port's tag reservation, and a port-level `publishTag` cannot take a second one:
the attempt is refused, reported on `stderr` and the tag dropped, rather than retargeting the open
reservation and handing the downstream block ring slots nobody wrote. Publish through the span —
`outSpan.publishTag(map, offset)` in `processBulk` — not through the port, and not via
`this->publishTag(map)`, which goes to the ports directly outside processOne dispatch. The framework
follows the same rule for the settings `applySettings()` forwards on the work call that ends the
stream: they leave on that call, through the span it holds, before the end-of-stream tag.

---

## Automatic tag forwarding

The framework forwards tags from input to output automatically. Every block forwards every key of
every tag it reads, on every synchronous output. A block changes this in four ways:

- `DroppedTagKeys<"key", ...>` drops the listed keys and forwards the rest.
- `BackwardTagPropagation` puts every tag of a chunk on the chunk's first output sample.
- `NoTagPropagation` forwards no tag. The block publishes its own.
- An override of `forwardTags()` replaces the default forwarder (see below).

### Where a forwarded tag lands

```cpp
struct MyBlock : gr::Block<MyBlock> { ... };
```

A block keeps tag offsets (`keepsTagOffsets()`) where it has only synchronous stream ports and
declares none of `Resampling<>`, `Stride<>`, `NoTagPropagation`, `BackwardTagPropagation` or a
`forwardTags()` override. Its default forwarder places every tag of the call's chunk at the tag's
own offset, before the call. On a block with `Stride<>` or an asynchronous stream port, the
framework ends a chunk at the next tag, so a tag sits on the first sample of its chunk, and the
forwarder places it on the call's first output sample.

| Block declares                              | A tag of a call leaves at                                                      |
| ------------------------------------------- | ------------------------------------------------------------------------------ |
| nothing, with only synchronous stream ports | its own offset                                                                 |
| `Stride<>` or an asynchronous stream port   | the call's first output sample                                                 |
| `Resampling<>`                              | output index `floor(i * output_chunk_size / input_chunk_size)`, after the call |
| `BackwardTagPropagation`                    | output index 0, the chunk's first output sample                                |
| `Stride<>`, skipped sample                  | the next chunk's first output sample, or the end-of-stream index               |

On a block declaring `Resampling<>`, `i` is the tag's input index among the samples the call
consumed. The forwarder places those tags after the call, merged by index with the block's own. The
tags of the samples the call did not consume stay on the input for a later call. A tag past the
samples an output span publishes leaves at the end of what it publishes. On a block declaring an
overlapping `Stride<>`, the chunk that consumes a tag's sample forwards it, and a chunk that only
reads the sample does not. When a stream ends, the tags at or past the block's read position, up to
the first end-of-stream tag on any synchronous input, leave at the end-of-stream index.

```
  input:    ▽           ▼              ▽
            │           │              │
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─
            0  1  2  3  4  5  6  7  8  9  10

  chunk 1:  ├───────────┤                          processedIn=4  (▽ at 0, next tag at 4)
  chunk 2:              ├──────────────┤           processedIn=5  (▼ at 4, next tag at 9)
  chunk 3:                             ├─────┤     processedIn=2  (▽ at 9, end of data)

  output:   ▽           ▼              ▽           each tag at its own sample
```

`min_samples` on an input port and `input_chunk_size > 1` on a resampling block put a floor under
the chunk. A tag closer than that to the chunk's start stays inside the chunk. A block that keeps
tag offsets places it at its own offset, and its settings take effect from the chunk's first sample.
On a resampling block the forwarder places it at its own mapped sample, and its settings take effect
after the call that consumes it, once. On a block with `Stride<>` or an asynchronous stream port the
tag stays on the input and returns in the next call at a negative relative index. It leaves there,
once, on that call's first output sample, and its settings take effect from that call's first
sample.

A block that keeps tag offsets forwards the tags of the whole chunk before the call. Where it
consumes fewer samples than the chunk holds, a tag the call did not consume leaves again at each
later call that reads it. Where it publishes fewer samples than it consumes, a tag past what it
publishes keeps its offset. A tag behind the read position that no span retired does not leave at
the end of the stream.

A 4:1 decimator (`Resampling<4, 1>`) that consumes every chunk it reads, with tags at input samples
0, 1, 2, 5, 6 and 9:

```
  input:    ▽  ▽  ▽        ▽  ▽        ▽         input_chunk_size = 4
            │  │  │        │  │        │
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─
            0  1  2  3  4  5  6  7  8  9  10 11

  output:   ▽▽▽            ▽▽          ▽         floor(i / 4): 0, 0, 0, 1, 1, 2
            │              │           │
  samples: ─┬──────────────┬───────────┬─
            0              1           2
```

### The block author's part

The framework places a tag by the block's declarations alone. It cannot check sample positions.
**A tag must belong at the output sample the table above gives it.** Nothing checks this rule. A
block whose output samples do not stand there declares `NoTagPropagation` and publishes its own tags,
or supplies `forwardTags()`. Such blocks are

- a delay, which shifts every sample,
- a block that drops samples without declaring `Resampling<>`, as one that keeps every Nth sample,
- a block that reframes its input, as a packet framer or a deinterleaver,
- a block with an asynchronous stream port, and
- a rate change whose tags must account for a filter's group delay.

### Backward tag propagation (`BackwardTagPropagation`)

```cpp
struct Spectrum : gr::Block<Spectrum, gr::BackwardTagPropagation, gr::Resampling<1024U, 1U>> { ... };
```

Every tag of the chunk leaves at the chunk's first output sample. A block whose output describes its
whole input chunk, a spectrum or a DataSet for instance, declares it. On a block with an
`output_chunk_size` of 1, the mapped placement gives the same result.

```
  input:    ▽     ▼  ▽
            │     │  │
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─     input_chunk_size = 10
            0  1  2  3  4  5  6  7  8  9

  chunk:    ├─────────────────────────────┤     processedIn=10, all tags in window
                           │
                      decimate ×10
                           │
                           ▼

  output:   ▽▼▽                                all three tags at output position 0
            │││
  samples: ─┬──┬─
            0
```

### No tag propagation (`NoTagPropagation`)

```cpp
struct MyBlock : gr::Block<MyBlock, gr::NoTagPropagation> { ... };
```

The framework does not forward any tags. The block handles tag propagation entirely in `processBulk`
or via a custom `forwardTags()` override. A block composed at compile time (`MergeByIndex`, `Merge`,
`SplitMergeCombine`, `FeedbackMergeByIndex`, `FeedbackMerge` and their tap variants) declares
`NoTagPropagation` when any member does or supplies its own `forwardTags()`, as a tag passes through
every member in series.

### Which keys a forwarded tag carries (`DroppedTagKeys`)

```cpp
struct Plain : gr::Block<Plain> { ... };                                // every key
struct Shift : gr::Block<Shift, gr::DroppedTagKeys<"foo:freq_est">> { ... }; // every key but foo:freq_est
```

A forwarded tag carries every key it arrived with, in every domain. The multi-input dedup is the
same for every key.

`DroppedTagKeys<"key", ...>` is the opt-out. A block lists the keys whose values do not describe its
output. A block that shifts the carrier frequency, for instance, lists the key that states a
frequency estimate. The default forwarder drops each listed key, on every chunk and at the
end-of-stream index, at the top level of a tag and inside the map a `gr:trigger_meta_info` key
holds. A `forwardTags()` override publishes what it writes, and the list reaches it only through
`filterAndSubstituteTag()`. A block composed at compile time drops the keys of every member. A block
declares at most one list. A listed key matches the whole key, and a reserved key is listed as
`gr:name`. The build refuses a listed key with a leading colon or a space.

The framework fixes positions, not values. A key whose value a block's output invalidates is listed
in `DroppedTagKeys` or rewritten in `forwardTags()`. One value changes on the way: on a block
declaring `Resampling<>`, `gr:sample_rate` leaves scaled by the chunk ratio. A key a setting of the
block is coupled to leaves with the value it arrived with, whatever the setting holds.

```
  input tag:  {gr:trigger_name: "burst", record_id: "r7", foo:gain: 10}

  forwarded ─►  {gr:trigger_name: "burst", record_id: "r7",   every key passes as it arrived,
                 foo:gain: 10}                                foo:gain included, though the block's
                                                              gain setting is coupled to it
```

### Custom tag forwarding — `forwardTags()`

For full control, override `forwardTags()` in your block. The framework calls it instead of the
default forwarding logic, at three points:

- for each chunk, with `processedIn` the number of samples the chunk holds;
- for the trailing span that `processEpilogue()` receives, on a block that has one;
- once more when an input's stream ends, with `processedIn` 0.

In that last call the input and output spans hold no samples. An input span's `tags()` holds the tags
at or past the block's read position, up to the first end-of-stream tag on any synchronous input,
at a relative index of 0 or more. The input's own end-of-stream tag is among them, and an override
drops that key, since `publishEoS()` publishes the block's own. Tags behind the read position that no
span retired appear at a negative index, and an override skips them. Offset 0 of an output span is
the end-of-stream index. On a block with only synchronous ports and no `processEpilogue()`, the end
call is the only call with `processedIn` 0.

On a block declaring `Stride<>`, the override is not called for the samples a stride skips. Their
tags stay in the input and reach the next chunk's call at a negative relative index. An override that
reads a wider window than `tags(1)` sees these tags at `relIndex` from `-(stride - input_chunk_size)` to
-1, and no earlier call has handed them over. It forwards them at offset 0 and skips only the tags
before that range, which the previous chunk's call saw. When the stream ends before that chunk, they
reach only the end call, at a negative index. The example below skips them there and does not forward
them.

The example adds a key to every tag it forwards, at a chunk and at the end of the stream:

```cpp
struct MyBlock : gr::Block<MyBlock> {
    template<typename TInputSpans, typename TOutputSpans>
    void forwardTags(TInputSpans& inputSpans, TOutputSpans& outputSpans, std::size_t processedIn) {
        const auto forward = [&outputSpans](property_map tagMap) {
            tagMap.erase(gr::tag::END_OF_STREAM.key()); // publishEoS() publishes the block's own
            if (tagMap.empty()) {
                return;
            }
            tagMap["my_key"] = "my_value";
            for_each_writer_span([&tagMap](auto& outSpan) { outSpan.publishTag(tagMap, 0UZ); }, outputSpans);
        };
        for_each_reader_span([&](auto& inSpan) {
            if (processedIn == 0UZ) { // the end of the stream: the tags at or past the read position
                for (const auto& [relIndex, tagMapRef] : inSpan.tags()) {
                    if (relIndex >= 0) {
                        forward(tagMapRef.get());
                    }
                }
            } else { // a chunk: the tags the input span retires
                for (const auto& [relIndex, tagMapRef] : inSpan.tags(1UZ)) {
                    forward(tagMapRef.get());
                }
            }
        }, inputSpans);
    }
};
```

`forwardTags` is detected via `requires` at compile time — no registration needed.
`BackwardTagPropagation` and `DroppedTagKeys` only affect the default implementation; a user
`forwardTags` replaces it entirely.

An override's input span retires only `tags(1)`, unless the block declares `BackwardTagPropagation`.
An override that reads a wider window — `in.tags(processedIn)`, `in.tags()`, `in.rawTags` — sees
every deferred tag twice: once at its true positive offset in the chunk it lies in, and again at a
negative offset in the chunk after that. Such an override must skip `relIndex < 0` and handle each
tag where it lies:

```cpp
for (const auto& [relIndex, tagMapRef] : in.tags(processedIn)) {
    if (relIndex < 0) {
        continue; // an earlier chunk already forwarded this one
    }
    // ... map the offset, publish
}
```

### Rate-changing blocks

On a block declaring `Resampling<>`, the default forwarder maps a tag at input index `i` of the
samples a call consumed to `floor(i * output_chunk_size / input_chunk_size)`. A block that wants
another mapping overrides `forwardTags()`. `gr::blocks::filter::RationalResampler` is the reference
pattern: it maps each input offset by `floor(i*L/M + 1/2)`, skips `relIndex < 0`, and holds a tag
whose output sample this call does not produce
rather than moving it to an offset that is not its own, publishing it in the call that does produce
that sample. Such a block calls `scaleSampleRateByChunkRatio()` on the tags it forwards.

---

## Multi-input deduplication

For blocks with multiple input ports, identical tags from fan-out (same index and same map content)
are deduplicated automatically by the default forwarding logic.

```
  upstream block
       │
       ├──────────┬──────────┐    fan-out: same tag on both connections
       ▽          ▽          │
  ┌──────────┬──────────┐    │
  │  in[0]   │  in[1]   │    │
  │  ▽ A     │  ▽ A     │    │    tag A appears on both ports (duplicate)
  │  ▼ B     │          │    │    tag B only on port 0 (unique)
  └──────────┴──────────┘    │
       │                     │
  dedup: A forwarded once, B forwarded once
       │
       ▽
  ┌──────────┐
  │  out[0]  │
  │  ▽ A     │    1× A (deduped) + 1× B (unique) = 2 output tags
  │  ▼ B     │
  └──────────┘
```

The dedup uses a stack-local array (capacity 8) — no heap allocation. The comparison early-returns
on first key-value mismatch. If more than 8 unique tags appear in a single work call, tags beyond
the capacity are forwarded without dedup tracking (redundant but correct — no data loss).

---

## Settings synchronisation

A tag sets a setting only where the setting is coupled to the tag's key. Blocks declare no coupling;
the application or the graph sets it per block instance:

```cpp
auto& gain = graph.emplaceBlock<Gain>();
if (auto coupled = gain.setTagCouplings({{"sample_rate", std::string("gr:sample_rate")}}); !coupled) {
    // each entry of coupled.error() names a refused setting and the reason
}
```

When a tag carrying `gr:sample_rate` arrives at the first sample of a work call, the framework checks
its value as a call's and stages it for `sample_rate`. The block's `applySettings()` runs before the
next processing call. A tag at the value `get()` returns stages nothing. A value the check refuses goes out
as an error notification, and the tag goes on downstream. With no coupling, no setting takes any tag, whatever its name. The key may be in
any domain: a block's setting may follow `foo:level` that an out-of-tree block writes.

```
  upstream                  downstream
  ┌──────────┐              ┌──────────┐
  │ TagSource│──▽───────────│ Gain     │
  │          │  tag:        │          │
  │          │  {gr:sample_ │ sr=48000 │    ← sample_rate coupled to gr:sample_rate
  │          │   rate: 48k} │          │
  └──────────┘              └──────────┘
```

The coupling table maps a setting's name to a key. A setting follows at most one key, and one key
may set several settings of a block. An entry with an empty key removes a coupling, and
`block.tagCouplings()` reads the table. The table belongs to the block, and a reset leaves it as
it is. A name of the bare group's settings couples member 0's key. The call returns a refusal for a
name that is not a writable setting, a key that is not a string or not `domain:name`, and the two
keys the framework acts on itself: `gr:end_of_stream` and `gr:reset_default`. Each entry
names the setting and the key, and the table stays as it was.

A coupled setting follows its key from the call that couples it. A caller's `set()` holds until the
next tag that carries the key, and a value a tag sets is no value a caller gave: a save does not
write it.

A graph file holds the table in a block's `tag_couplings` map, and the loader applies it after the
block's `parameters`:

```yaml
blocks:
  - id: Gain
    parameters:
      name: gain
      sample_rate: !!float32 1000
    tag_couplings:
      sample_rate: "gr:sample_rate"
```

`block.setTagCouplings()` changes the table at run time, and `block.tagCouplings()` reads it.
`grinfo` reports each setting's coupled key, read from the instance it inspects, as `tagKey`.

The framework writes no tag for a setting, whether a caller or a tag set it. A block that hands a
value downstream names it with `this->forwardTag()`, and the framework publishes it at the first
sample of the block's next output:

```cpp
struct Settings {
    gr::Annotated<float, gr::Unit<"Hz">> sample_rate = 1.f;

    GR_MAKE_REFLECTABLE(Settings, sample_rate);
} settings;

void applySettings(const Settings& s, const Settings& old) {
    if (s.sample_rate != old.sample_rate) {
        this->forwardTag(gr::tag::SAMPLE_RATE.key(), s.sample_rate.value);
    }
}
```

The hook also runs when the graph takes the block, with the declared defaults in `old`; the framework drops
what it forwards then, so the block above publishes one tag per later change of its rate and none
at the start. A block whose first sample must carry its rate publishes that tag in its work function.

When `applySettings()` throws or fails a key, every key of that apply takes back its earlier value, an
error notification names each, and the block publishes none of the settings the failing call forwarded.
The tag leaves with the value it arrived with.

A block that republishes _every_ input tag itself, whatever its keys, declares `NoTagPropagation`.
The framework then forwards no tag on its own:

```cpp
struct Delay : gr::Block<Delay, gr::NoTagPropagation> {
    work::Status processBulk(InputSpanLike auto& input, OutputSpanLike auto& output) {
        const auto n = std::min(input.size(), output.size());
        for (const Tag& tag : input.rawTags) {
            if (tag.index >= input.streamIndex && tag.index - input.streamIndex < n) {
                output.publishTag(tag.map, tag.index - input.streamIndex);
            }
        }
        std::ranges::copy(input.first(n), output.begin());
        std::ignore = input.consume(n);
        output.publish(n);
        return work::Status::OK;
    }
};
```

To rewrite or filter keys per tag rather than republish them all, override `forwardTags()` (see
"Custom tag forwarding" above).

### `gr:sample_rate` and decimation/interpolation

`gr:sample_rate` receives special treatment in blocks with
`input_chunk_size != output_chunk_size`. When the framework forwards `gr:sample_rate`, it
automatically adjusts the value based on the chunk ratio:

```
  forwarded sample_rate = input sample_rate × (output_chunk_size / input_chunk_size)
```

Example — a 10× decimator:

```
  input:    ▽ sample_rate = 10000 Hz
            │
  samples: ─┬──┬──┬──┬──┬──┬──┬──┬──┬──┬─    input_chunk_size = 10
            │  │  │  │  │  │  │  │  │  │
           ─┴──┴──┴──┴──┴──┴──┴──┴──┴──┴─
                           │
                      decimate ×10
                           │
                           ▼
  output:   ▽ sample_rate = 1000 Hz           10000 × (1/10) = 1000
            │
  samples: ─┬──┬─                             output_chunk_size = 1
            │  │
           ─┴──┴─
```

The ratio is applied to the `gr:sample_rate` a forwarded tag carries, so the block's own stored
`sample_rate` stays at the rate it is fed. A block with no `sample_rate` setting also publishes the
scaled value. A block's own forwarded settings leave as the block named them. The decimation block only needs
to set `input_chunk_size` in `applySettings`:

```cpp
void applySettings(const Settings& s) {
    this->input_chunk_size = s.decim;
}
```

---

## Trigger tags

Trigger tags mark events in the data stream — zero crossings, threshold exceedances, external timing
events, etc. They use a standard key triplet (plus one optional key):

| Key                    | Type            | Required | Description                                               |
| ---------------------- | --------------- | -------- | --------------------------------------------------------- |
| `gr:trigger_name`      | `std::string`   | yes      | event identifier (e.g., `"zero_crossing"`, `"beam_in"`)   |
| `gr:trigger_time`      | `uint64_t` (ns) | yes      | UTC timestamp of the event                                |
| `gr:trigger_offset`    | `float` (s)     | yes      | sub-sample time offset (compensating analog group delays) |
| `gr:trigger_meta_info` | `property_map`  | optional | additional key-value pairs (detector ID, amplitude, ...)  |

**These keys should always appear together** (except `gr:trigger_meta_info` which is optional). A
trigger tag without `gr:trigger_time` or `gr:trigger_offset` is incomplete. The keys nested in
`gr:trigger_meta_info` are free-form.

```
  trigger tag at sample 5:
  {
      "gr:trigger_name":      "beam_extraction",
      "gr:trigger_time":      1720000000000000000,    // UTC ns
      "gr:trigger_offset":    -0.000023,              // 23 µs before this sample
      "gr:trigger_meta_info": {
          "beam_id":    "PS_B1",
          "intensity":  2.3e12
      }
  }
```

Trigger tags are forwarded like any other tag. Downstream blocks (e.g.,
`StreamToDataSet`) use the trigger matcher API to start/stop data acquisition windows based on
trigger patterns.

---

## Standard tag keys (`gr:`)

The keys below are `gr:` keys, which GNU Radio defines. Any block may write or read one with the
meaning given here. Code names them through the constants in `gr::tag::`
(`gr::tag::SAMPLE_RATE.key()` is `"gr:sample_rate"`, and `.name()` is `"sample_rate"`). A bare
`sample_rate` is a key in the default domain and means nothing to the framework. Keys are aligned
with the [SigMF](https://sigmf.org/) standard for interoperability. A `gr:` key's value has the
type of its constant's `value_type`.

### Signal description

| Key                  | Type          | Unit | Purpose                                                               |
| -------------------- | ------------- | ---- | --------------------------------------------------------------------- |
| `gr:sample_rate`     | `float`       | Hz   | sample rate of the stream. Auto-adjusted by decimation/interpolation. |
| `gr:frequency`       | `double`      | Hz   | center frequency of the stream                                        |
| `gr:signal_name`     | `std::string` |      | human-readable signal name (e.g., `"RF_pickup_H"`)                    |
| `gr:num_channels`    | `gr::Size_t`  |      | interleaved channel count (for multi-channel streams)                 |
| `gr:signal_quantity` | `std::string` |      | physical quantity (e.g., `"voltage"`, `"current"`)                    |
| `gr:signal_unit`     | `std::string` |      | SI unit (e.g., `"V"`, `"A"`)                                          |
| `gr:signal_min`      | `float`       | a.u. | physical minimum (e.g., DAQ range lower bound)                        |
| `gr:signal_max`      | `float`       | a.u. | physical maximum (e.g., DAQ range upper bound)                        |

### Data quality

| Key                    | Type         | Purpose                                                                           |
| ---------------------- | ------------ | --------------------------------------------------------------------------------- |
| `gr:n_dropped_samples` | `gr::Size_t` | number of samples lost before this point, counted on the stream it was written on |

A rate change does not rescale `gr:n_dropped_samples`. Its readers count the tags.

### Triggers

| Key                    | Type           | Unit | Purpose                    |
| ---------------------- | -------------- | ---- | -------------------------- |
| `gr:trigger_name`      | `std::string`  |      | event identifier           |
| `gr:trigger_time`      | `uint64_t`     | ns   | UTC timestamp of the event |
| `gr:trigger_offset`    | `float`        | s    | sub-sample time correction |
| `gr:trigger_meta_info` | `property_map` |      | additional event metadata  |

### Lifecycle

| Key                | Type   | Purpose                                                      |
| ------------------ | ------ | ------------------------------------------------------------ |
| `gr:reset_default` | `bool` | reset block settings to stored defaults                      |
| `gr:end_of_stream` | `bool` | signals end of data — downstream blocks transition to `DONE` |

A block acts on a `gr:reset_default` tag as it acts on a call to `block.resetDefaults()`. The
value is not read. A bare `reset_default` key acts on nothing. The stored defaults are the values in
force when the block's initialization ended.

A reset, by tag or by call, sets every stored default, as one call that gives each key, and then
runs the block's `reset()`. The coupling table stays as it was.

A reset and the other keys of its tag apply as one change: the restored defaults, with the tag's own values over them.
`applySettings()` hears of that change once. A `resetDefaults()` call made while a reset tag waits to apply serves both,
and the block's `reset()` runs once. The tag forwards downstream, and each block it reaches acts on it. A host that
wants one block to reset calls that block's `resetDefaults()`, not a tag.

### Transmit bursts

| Key          | Type       | Unit | Purpose                                                                  |
| ------------ | ---------- | ---- | ------------------------------------------------------------------------ |
| `gr:tx_sob`  | `bool`     |      | the tagged sample is the first of a transmit burst                       |
| `gr:tx_eob`  | `bool`     |      | the tagged sample is the last of a transmit burst                        |
| `gr:tx_time` | `uint64_t` | ns   | UTC time at which the tagged sample, a burst's first, leaves the antenna |

A transmit sink reads these; a receive graph's `gr:trigger_time` is never read as a transmit time. The names are GNU
Radio 3's; `gr:tx_time` is one count of nanoseconds, as `gr:trigger_time` is, where GNU Radio 3 carried whole and
fractional seconds.

---

## Performance notes

- `inputTagsPresent()` is a `bool` check — zero cost in the no-tag path.
- `input.tags()` is a lazy view over the tag buffer — no allocation.
- The processOne dispatch loop checks `_outputTagPending` (a `bool`) per sample —
  branch-predicted, no overhead when no tags are published.
- Single-input blocks skip multi-port dedup entirely (compile-time `if constexpr`).
- Tag-heavy workloads (tag every sample) are dominated by `property_map` allocation, not by the
  forwarding logic.
