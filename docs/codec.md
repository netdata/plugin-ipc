# Codec Specification

## Purpose

The Codec is a set of pure byte-layout converters between wire format and
strongly typed language structures. It is parallel to Level 1, not on top of
it. It has zero dependency on transports, connections, or I/O.

The Codec exists so that:

- Level 1 integrators who build their own event loops can encode/decode
  typed payloads directly, without Level 2.
- Level 2 convenience wrappers can use the same encode/decode functions
  internally, without duplicating wire-format knowledge.
- The wire-format contract between implementations (C, Rust, Go) is defined
  once and implemented identically in all languages.

## Scope

The Codec owns:

- Typed request/response payload encoding (typed structure to opaque bytes)
- Typed request/response payload decoding (opaque bytes to ephemeral typed
  view)
- Response builder pattern (handler-friendly construction of response payloads)
- Explicit copy/materialize helpers (ephemeral view to owned structure)
- Validation of payload structure during decoding (offsets, lengths, bounds,
  trailing NUL)

The Codec does NOT own:

- The outer message envelope (that is Level 1 wire framing)
- The batch item directory (that is Level 1 batch framing)
- Transport, connection management, or I/O (Level 1)
- Client context, retry policy, managed server dispatch (Level 2)
- Snapshot refresh, caching, or lookup (Level 3)

## Principles

### 1. Pure byte manipulation

Codec functions are stateless and side-effect-free. They take bytes in and
produce typed structures out, or take typed structures in and produce bytes
out. They never touch sockets, files, shared memory, or any OS resource.

A caller can encode a request without having a connection. A caller can
decode a response from bytes stored in a file. The codec is completely
independent of how the bytes were transported.

### 2. Zero-allocation decoding

The default decode path must not allocate heap memory. Decoded structures
are non-owning views that borrow directly from the underlying message
bytes. String fields are exposed as pointer + length into the payload
buffer, not as copied strings.

Allocation happens only when the caller explicitly requests an owned copy
via a materialize/copy helper.

### 3. Ephemeral view lifetime

Decoded request and response views are tied to the lifetime of the
underlying payload buffer. The exact lifetime therefore depends on which
layer owns that buffer:

- Direct Codec use or Level 1 caller-managed buffers:
  the view is valid only while the caller keeps the backing buffer alive
  and unchanged
- Level 2 client calls:
  response views are valid until the next typed call on the same client
  context, or until that client is closed, unless the service-kind-specific
  contract states a narrower lifetime
- Level 2 server callbacks:
  request views and response builders are valid only for the duration of
  the current callback invocation

The library is free to reuse internal buffers aggressively. Once the
buffer owner is allowed to reuse or release the underlying storage, the
view becomes invalid immediately.

This is a hard contract, not a suggestion.

### 4. Self-contained payloads

Each request or response payload is self-contained. It can be decoded
in isolation given only its byte range. The outer envelope identifies
where the payload starts and how long it is. The codec takes that byte
range and decodes it without needing any other context.

Each payload type has its own internal structure:

- A fixed payload-local header with scalar fields
- Offset + length pairs for variable-length fields
- A packed variable data area

The outer envelope never knows the inner payload field layout. The codec
never knows about the outer envelope.

### 5. String field representation

Variable-length string fields inside payloads are represented by:

- offset (u32): byte position within the payload's variable data area
- length (u32): byte length of the string data

The pointed bytes must also terminate with NUL (`\0`). This provides:

- C: cheap direct pointer to NUL-terminated string
- Rust/Go: O(1) slice without scanning for the terminator

Encoders must always append the trailing NUL. Decoders must validate its
presence.

### 6. Strict validation on decode

Decoders must reject invalid payloads rather than producing corrupt views.
Validation includes:

- Payload shorter than the fixed method-local header: reject
- Offset + length for any field exceeding payload bounds: reject
- Overlapping field regions where the schema forbids them: reject
- String field missing the required trailing NUL: reject
- Unknown or unsupported layout_version: reject

Decode failure is reported as an error, not as a partial view.

## Naming discipline

Type and function naming must make the ephemeral/zero-copy nature of
decoded structures impossible to miss:

- **Owned encode input types**: no special suffix. These are the caller's
  data that the encoder consumes. Example: `IpToAsnRequest`.
- **Decoded borrowed types**: always contain `View` in the type name.
  Example: `CgroupsSnapshotResponseView`, `StrView`, `CStringView`.
- **Response builders**: always contain `Builder` in the type name.
  Example: `CgroupsSnapshotResponseBuilder`.
- **Explicit copy helpers**: use verbs like `copy`, `materialize`,
  `to_owned`. Example: `CgroupsSnapshotView.ToOwned()`.

Comments and documentation on all `View` types must explicitly state:

- borrowed / ephemeral
- the exact lifetime contract for the owning layer
- copy immediately if the data is needed later

## Encode contract

An encode function takes a typed request or response structure and produces
a self-contained byte payload. The encoder:

- Writes the fixed method-local header (scalar fields)
- Writes offset + length pairs for each variable-length field
- Appends packed variable data (strings with trailing NUL, properly aligned)
- Returns the complete payload bytes

The produced bytes are ready to be passed to Level 1 as an opaque payload.

## Decode contract

A decode function takes a byte range (the payload identified by the outer
envelope or batch item directory) and produces an ephemeral typed view.
The decoder:

- Validates the payload against the rules in Principle 6
- Constructs a view structure whose fields point into the payload bytes
- Returns the view (or an error if validation fails)

The returned view borrows the payload bytes. It is valid only as long as
the underlying buffer is valid. The owning layer defines the exact
lifetime contract for that buffer.

## Builder contract

A response builder provides handler-friendly construction of response
payloads without exposing raw offset/length bookkeeping. The builder:

- Is initialized or reset before use
- Accepts scalar fields via set methods
- Accepts variable-length fields (strings) via set/append methods
- Internally manages: packed variable-data placement, offset + length
  assignment, trailing NUL insertion, alignment and padding
- Finalizes into one self-contained payload via a finish method

Handlers interact only with semantic field names. They never compute
offsets, manage padding, or insert NUL terminators manually.

## Per-service-kind contract files

Each message type gets its own specification file that defines:

- The method code (used in the outer header's code field)
- Request payload wire layout: field names, types, byte offsets, sizes,
  variable-length field rules
- Response payload wire layout: same structure
- Per-item business/method result codes (inside the response payload,
  not in the outer envelope)
- Batch semantics: whether batches of this method type are valid, and
  if so, what item correlation rules apply

These contract files are the authoritative reference for cross-language
interoperability. If C, Rust, and Go implementations produce different
bytes for the same typed input, the implementation that does not match
the contract file is wrong.

## Testing requirements

The Codec must have:

- **High test coverage** (90%+ enforced): every encode path, every decode path, every
  builder method, every validation rule, in all languages.
- **Round-trip tests**: encode a typed structure, decode the resulting
  bytes, verify the view matches the original data. For every method
  type, in every language.
- **Cross-language interop tests**: encode in language A, decode in
  language B. For every pair of supported languages. For every method
  type.
- **Fuzz testing**: feed arbitrary byte sequences to every decode function.
  No input may cause a crash, panic, or undefined behavior. Invalid
  inputs must produce a clean error.
- **Boundary tests**: payloads at exactly the minimum valid size, at the
  maximum negotiated size, with zero-length strings, with maximum-length
  strings, with the maximum number of batch items.
- **Validation rejection tests**: explicit tests for every rejection rule
  in Principle 6 — truncated payloads, out-of-bounds offsets, missing
  NUL terminators, overlapping fields.

No exceptions. The Codec is the interoperability contract between
implementations. Any mismatch is a protocol violation.
