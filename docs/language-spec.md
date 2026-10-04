# DiscoC language specification

## Status and scope

DiscoC is a small freestanding systems language, not an implementation of ISO C.
Its C-like spelling does not import C's implicit conversions, undefined signed
overflow, object layout, library, or calling conventions.

This document is the contract for the ordered language refactoring.
It distinguishes three statuses:

- **Current:** behavior implemented by the current frontend or GSU backend.
- **Planned:** a language requirement of the refactoring, not yet available.
- **Open:** a policy or representation that must be settled and tested before
  the relevant feature can be described as supported.

Code examples are **Current** unless explicitly marked otherwise. Acceptance by
the parser alone is not evidence of correct code generation. Known discrepancies
below are implementation gaps, not language features to preserve.

The source language and a target's machine ABI are separate contracts. For
backend/register conventions, see [architecture.md](architecture.md); for
loading and startup, see [gsu-loading.md](gsu-loading.md). A linked `.bin` is a
fixed-origin processor payload, not a complete SNES ROM or a self-relocating
routine.

## 1. Translation units and target selection

**Current.** One `.dc` file is a translation unit. It may contain function
definitions, prototypes, structures, enums, constants, RAM globals and ROM data.
`module`/`import` headers share interfaces, not pasted source. Declarations are terminated by `;` where shown below.
No preprocessor, header inclusion, C typedef grammar, namespaces, variadic calls,
or dynamic allocation is specified. Transparent `type` aliases and explicitly
linked freestanding libraries are available (sections 15 and 17).

The build selects the processor, rather than the source file:

```sh
discc --target gsu main.dc -o main.o
discc --target spc700 main.dc --emit-ir
```

GSU is the default and has a code-generation backend. SPC-700 currently has a
target model and IR inspection only; object/assembly emission reports that no
backend is available. It must not silently generate GSU code for SPC-700.

Placement is build configuration, not source semantics:

```bash
discc --target gsu --memory-mapping lorom --execution-memory ram --origin 0x706000 program.dc -o program.o
```

The former source-level `set` directives are rejected with a migration diagnostic.
The same target/placement defaults can be stored in a `discoc.toml`
[project manifest](project-manifest.md); explicit CLI options take precedence.
This remains build configuration and does not change source-language semantics.

LoROM is the default. HiROM remains an explicit mapping choice. Execution
memory defaults to ROM; RAM execution is GSU-only. An explicit origin takes
precedence over mapping/memory defaults. When execution memory is explicit,
the origin must belong to that region. All linked units must have compatible
target/mapping settings; a linker `--origin` override changes their effective
execution origin together. These options do not configure bus ownership,
load code into RAM, or reserve a stack.

## 2. Lexical rules

**Current.** Names are case-sensitive. Identifiers start with a letter or `_`
and continue with letters, digits, or `_`. Use ASCII identifiers for portable
source; a non-ASCII identifier/encoding policy is not yet specified.

Whitespace separates tokens. `//` starts a line comment; `/* ... */` is a
non-nesting block comment. An unterminated block comment is an error.
Floating-point literals are unsupported. Character/string rules are in section 12.

Integer tokens use decimal, hexadecimal (`0x`), or leading-zero octal notation.
For example, `12`, `0x0C`, and `014` denote the same value. A leading `-` is a
unary operator, not part of an integer token. Binary notation (`0b1100`) is also
supported consistently by analysis, constant evaluation and IR lowering.

Malformed/out-of-range literals are diagnosed. Literal range checks depend on
context; representability of a literal is separate from arithmetic wrapping.

## 3. Types and value representation

### Integer types

**Current.** `byte` and `word` are signed by default. `unsigned` selects their
unsigned counterpart. Their value widths are independent of the host compiler.

| Type | Value width | Storage bytes | Representable values |
| --- | --- | --- | --- |
| `byte` | 8 | 1 | -128 through 127 |
| `unsigned byte` | 8 | 1 | 0 through 255 |
| `word` | 16 | 2 | -32768 through 32767 |
| `unsigned word` | 16 | 2 | 0 through 65535 |

GSU word storage is little-endian. A byte-sized value still occupies a
word-aligned local/argument stack slot in the current GSU ABI; value size is
not stack-slot size.

`void` describes the absence of a return value. Plain `void` variables,
parameters, members, and ROM objects are rejected. An empty parameter list is
written `()`, not `(void)`. `void*` and `far void*` can carry opaque addresses,
including through arguments and returns. Cast to a complete object-pointer type
before dereferencing or doing arithmetic; a void pointer has no element stride.

**Current.** `bool` has one byte of storage and exactly two values: `false`
and `true`, represented by 0/1. Comparisons, logical operators, and logical
negation produce bool. Integer-to-bool conversion uses zero/nonzero, including
outside conditions; bool-to-integer conversion yields 0/1. A bool load normalizes
raw nonzero memory bytes, including volatile status bytes. `unsigned bool`
is invalid.

### Near/far data pointers

**Current.** `byte*`, `word*`, and pointers to structures use a two-byte near
address. `*` may be repeated. Near addresses are offsets interpreted in the
target's selected address space/bank, not complete SNES CPU addresses.
GSU RAMBR and ROMBR are distinct from the instruction bank PBR.

**Current.** Near is the default. A near address contains an offset within the
original bank context for its operation. Far data pointers identify both bank
and offset, allowing access outside that original data bank. Their four-byte
representation is supported in locals, structure fields, indirect storage,
arguments, and returns. Named far-object placement and interbank code calls are
not implemented; accepting `far` on a data pointer does not provide either.
A far address remains far even when its bank happens to match the current bank;
its type must not depend on runtime register contents.

These are three independent bank contexts, not one shared "current bank":

| Operation | Near bank context | Far operation |
| --- | --- | --- |
| Execute code | PBR | Interbank control transfer (not implemented) |
| Read ROM data | ROMBR | Select the object's ROM bank before reading |
| Read/write RAM data | RAMBR | Select the object's RAM bank before accessing |

Code in RAM can therefore use near RAM data without requiring PBR and RAMBR to
identify the same bank. `far` describes address reach, not scalar width: a far
`word` object still contains a two-byte value. The location of a pointer
variable and the bank of its pointed-to object must be represented separately.
The linker still places the complete payload in one program bank. Function
pointers are diagnosed rather than silently lowered as data pointers.

**GSU mechanism.** `IBT` can load an immediate bank selector; `ROMB` and `RAMB`
consume the source register. RAMB uses bit 0, so selectors 0/1 identify RAM banks
$70/$71. `LJMP Rn` takes the program bank from Rn (R8 through R13) and the offset
from the selected source register. `IWT` itself is not restricted to R8-R13.
LJMP resets instruction prefixes/selectors, updates the cache base to the
target's 16-byte boundary, and invalidates instruction-cache validity flags;
it does not reset every arithmetic status flag. `LINK` saves only a return
offset in R11, not the caller's program bank. These mechanisms can be checked
in the [Mesen GSU instruction implementation](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/Coprocessors/GSU/Gsu.Instructions.cpp).

The supported GSU-visible windows remain $00-$3F:$8000-$FFFF and
$40-$5F:$0000-$FFFF for ROM, and $70-$71:$0000-$FFFF for RAM. These are not
arbitrary SNES CPU addresses; cartridge capacity and host bus permissions still
apply. See [GSU loading](gsu-loading.md) for placement constraints.

**Current ABI invariant.** A compiler-generated far data access temporarily
selects the target bank and restores the original near data-bank context when
the access finishes, before any following near access, stack access, or call
that depends on it. This is not deferred until the next source statement:
backend-generated stack traffic also needs the original RAMBR. An unrelated
arithmetic instruction does not select a bank. Restoration is compiler-generated,
not automatic hardware behavior; raw assembly must manage its own bank state.
`discld --ram-bank 0|1` fixes the near RAM/stack context (default bank $70).
`--rom-bank` fixes the near ROM context (default execution bank for ROM code,
bank $00 for RAM code). These are link-time ABI settings, not snapshots of
arbitrary host register contents. Without `--init-runtime`, the host must set
RAMBR to the chosen baseline and initialize the stack before entry. Compiler
ROM reads select their configured bank; far reads restore it afterwards.

**Planned code ABI.** A far call must retain both the return offset and caller
PBR and restore them on return, while preserving the ordinary register/stack
contract and handling delay slots. A far jump alone is not a far-call ABI.
The called code must remain in its own PBR until it returns; restoring PBR after
each instruction would defeat the interbank transfer. This still requires its
own code-placement and call-lowering implementation.

**Current GSU pointer layout.** Near pointer values occupy two
bytes. Far pointer values occupy four bytes with two-byte alignment: bank at
byte +0, a reserved zero padding byte at +1, and the little-endian 16-bit offset
at +2/+3. RAM bank bytes are canonical $70/$71, not RAMB selectors 0/1.
Pointer storage width is independent of pointee width. Both pointer forms are
word-aligned, and a loaded far descriptor with nonzero padding is invalid.

For example, a far pointer stored at $70:$2000 can identify a word at $71:$1000.
Reading the pointer variable addresses its four bytes in bank $70; dereferencing
its value addresses the two-byte word in bank $71. Taking the address of that
local pointer produces a near pointer to a far-pointer value. Every pointer
level retains its own address reach; the maximum pointer depth is 32.
Prefix `far` qualifies the outermost pointer; `far` following `*` qualifies
that level. For example:

```c
far word* object = (far word*)0x711000;
word* far* local_slot = &object; // near pointer to a far word-pointer value
far word* far* remote_slot = (far word* far*)0x710300;
*remote_slot = object;          // descriptor stored in bank $71
**remote_slot = 42;             // word stored at $71:1000
```

`&*object` preserves the address type. Taking the address of a local ROM-pointer
variable still produces a RAM pointer to that variable; the pointee space of
its stored value does not change its own storage space.

**Current conversions.** Near-to-far widening requires an explicit cast
when the pointed-to type and address space are preserved. It obtains the bank
from the matching original near address-space context, not a temporary bank
selected for another far access. For instance, a near RAM offset $1234 in bank
$70 becomes the far address $70:$1234. Far-to-near narrowing requires an explicit
conversion and validation that the bank and offset are representable in the
destination near context. $71:$1234 cannot become near $1234 when the near RAM
bank is $70. Pointer-to-pointer conversions must not rewrite inner pointer
representations implicitly. A target without far addressing must issue a
diagnostic rather than silently truncate it.

An explicit pointer cast may change the leaf object type, but cannot change
address space, depth, or an inner pointer's representation. Integer-to-pointer
construction requires a cast; far addresses require an explicit 24-bit literal.
A near pointer can be explicitly converted to/from `unsigned word`; signed
integers and far-pointer-to-integer conversions are rejected. Literal address
construction remains checked. There is no implicit integer-to-pointer rule.

`null` is contextually typed. Near null is offset zero; far null is the all-zero
bank/offset descriptor. Null can be copied, returned and compared but cannot be
dereferenced or used for arithmetic. Near RAM offset zero is reserved; a far
pointer can address `$70:0000`. Equality compares both bank and offset for far
values. Pointer ordering and pointer differences remain unsupported.

**Current mandatory word alignment.** Every typed 16-bit data load or
store, including a `word` or two-byte pointer field, requires an even byte
address (`offset & 1 == 0`). A four-byte far-pointer descriptor is aligned to
two bytes so its offset field and word-sized copies are aligned. This rule
applies to near and far data and to both RAM and ROM, even when the backend
could synthesize a ROM word from two byte reads. The complete word must fit
in the selected bank window and available memory; an access may not straddle
a bank boundary.

Misaligned word accesses are **illegal language operations**, not optimizer
undefined behavior or presumed hardware exceptions. Diagnose a provable bad
address and check a dynamic one before the memory operation. Do not silently
round an address down, change the byte order, or split an illegal typed word
access into byte operations to bypass the rule. Explicit byte accesses remain
byte accesses and do not gain a two-byte alignment requirement.

**Hardware evidence.** Nintendo's [development manual, Book II](https://floating.muncher.se/bot/manual/book2_text.pdf)
defines LDW in section 9.46, printed page 2-9-66 (PDF page 223), and STW in
section 9.85, printed page 2-9-117 (PDF page 274). For RAM, the low byte uses A;
the high byte uses A+1 when A is even and A-1 when A is odd, equivalent to A XOR
1. Thus a raw word load at $70:$FFFF uses $70:$FFFF/$FFFE, not $71:$0000.
This instruction behavior is defined; the language deliberately disallows
relying on it for an unaligned word. ROM data uses byte-buffer instructions,
so the RAM word-instruction behavior must not be generalized to ROM reads.

**Current bank-aware arithmetic.** Under the default LoROM addressing
contract, far byte pointers to ROM traverse the canonical
$00-$3F:$8000-$FFFF windows, each holding 32 KiB. Incrementing past $FFFF
increases the bank and resumes at $8000; decrementing below $8000 decreases
the bank and resumes at $FFFF. Far byte pointers to RAM traverse
$70-$71:$0000-$FFFF, with 64 KiB per bank and the equivalent carry/borrow at
$FFFF/$0000. The stored RAM bank is $70/$71; the corresponding
RAMB selector is 0/1. Pointer arithmetic changes the pointer value, not the
current hardware bank registers; a later dereference selects the target bank.

The following examples use byte pointers, so +1/-1 mean one byte:

| Space | Operation | Result |
| --- | --- | --- |
| LoROM | $00:$FFFF + 1 | $01:$8000 |
| LoROM | $01:$8000 - 1 | $00:$FFFF |
| RAM | $70:$FFFF + 1 | $71:$0000 |
| RAM | $71:$0000 - 1 | $70:$FFFF |

For other pointee types, displacement is scaled by the pointee storage size.
Word-pointer arithmetic is scaled by two bytes and must preserve alignment.
It must also remain in the original bank: carry or borrow into a different
bank is **illegal even when both offsets are aligned and the pointer is far**.
This applies to addition, subtraction, and indexing, not only a unit increment.
The compiler must reject a provable crossing or check a dynamic displacement
before modifying the pointer or performing an access. This is a deliberate
language safety restriction, not a claim that aligned destinations are
misaligned hardware accesses. Selecting a word object in another bank through
an explicit, validated far address remains allowed; arithmetic cannot introduce
the bank change. The byte-pointer examples above do not authorize word accesses
at the odd addresses $FFFF shown there.

| Word-pointer operation | Result |
| --- | --- |
| LoROM $00:$FFFC + 1 | $00:$FFFE, allowed if the object/access is valid |
| LoROM $00:$FFFE + 1 | Illegal bank crossing; do not produce $01:$8000 |
| LoROM $01:$8000 - 1 | Illegal bank crossing; do not produce $00:$FFFE |
| RAM $70:$FFFE + 1 | Illegal bank crossing; do not produce $71:$0000 |
| RAM $71:$0000 - 1 | Illegal bank crossing; do not produce $70:$FFFE |

Allowed byte-pointer arithmetic must handle displacement across multiple
windows and check scaling and range before narrowing. Conceptually, a canonical LoROM address
has linear position `bank * 0x8000 + (offset - 0x8000)`; a RAM address has
position `(bank - 0x70) * 0x10000 + offset`. Apply the signed byte displacement
to that position, validate it, and then recover the bank and offset. Near
arithmetic does not acquire a bank carry or implicitly turn a near value far.

Only the offset wraps inside its window: bank numbers do not wrap around the
address-space endpoints. $3F:$FFFF + 1, $00:$8000 - 1, $71:$FFFF + 1, and
$70:$0000 - 1 are address errors in these canonical domains. Available cartridge
capacity can impose smaller limits. Access width and alignment must also be
validated; a valid first byte alone does not make an entire access valid.

This 32-KiB ROM traversal is a LoROM pointer policy, not a claim that all
GSU-visible ROM windows have that width. The $40-$5F full-bank ROM view remains
valid in the memory map. Far byte arithmetic in $40-$5F uses 64-KiB windows as
a separate domain: $40:$FFFF + 1 gives $41:$0000. It never wraps from $3F into
$40, or normalizes stored aliases implicitly. This preserves the full-bank ROM
view without implying multi-bank linker placement.

**Current address failures.** Provable invalid conversions/arithmetic/accesses
produce compile-time diagnostics. Dynamic failures terminate GSU execution with
`STOP; NOP` and a category in R6:

| R6 | Failure |
| --- | --- |
| 1 | Misaligned typed access/address |
| 2 | Invalid address, descriptor, span, or checked stack range |
| 3 | Illegal arithmetic bank crossing or domain escape |
| 4 | Far-to-near bank mismatch |
| 5 | Shift count outside 0..15 |
| 6 | Division or remainder by zero |

The check occurs before an illegal memory access or foreign-bank selection.
The host must treat this STOP as a failure, not resume after it; frames are not
unwound and ordinary completion is not implied. Generated `main` sets R6 to
zero on normal completion, including when only a callee uses checked pointers.
R6 remains a volatile scratch register during
execution, not a persistent error flag. These checks validate the GSU-visible
address domain, not actual cartridge capacity, object lifetime, or array bounds.

There is no `near` keyword: near reach is the default. Layout queries such as
`sizeof` are specified in section 11.

## 4. Address spaces, mutability, and observable accesses

**Current.** The default address space is RAM. These are independent dimensions:

- `rom`/`ram` specify the address space of the accessed object.
- `const` forbids modification through that access. It does not imply ROM
  placement or make arbitrary initializers constant expressions.
- `volatile` makes each evaluated load/store observable. It does not imply
  constness, atomicity, locking, or thread synchronization.

ROM is read-only even without const. A const RAM object still resides in RAM.
Locals and pointer variables have RAM storage, independently of their pointee
space. A scalar ROM local is rejected; declare ROM data globally.

Prefix qualifiers apply to the leaf object; qualifiers following `*` apply to
that pointer object. They survive member access, dereference, indexing,
conversion, and IR lowering:

```c
const ram word limit = 149;                // RAM, read-only program access
volatile byte* port = (volatile byte*)0x100; // mutable pointer to volatile data
word* const fixed = (word*)0x200;           // const pointer, mutable data
const volatile word* status = (const volatile word*)0x202;
```

Implicit conversions can add direct pointee const/volatile qualification but
cannot discard it. Explicit casts also cannot discard qualification, change
address space, or rewrite inner pointer representations. Unsafe nested
qualification conversions are rejected. Const locals and defined const RAM
globals require initializers; extern declarations do not initialize storage.

Two evaluated volatile reads are two memory reads, and each evaluated store
occurs. Passes must not erase, merge, duplicate through rematerialization, or
reorder them across other observable effects. IR memory instructions carry a
verified volatile flag. The GSU backend retains loads/calls at their definitions,
including unused volatile reads. An operand skipped by short-circuit evaluation
performs no access.

## 5. Expressions and operators

### Grammar and precedence

Groups are ordered from lowest to highest precedence:

| Group | Operators | Associativity |
| --- | --- | --- |
| Assignment | `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `\|=`, `^=`, `<<=`, `>>=` | right |
| Logical OR | `\|\|` | left |
| Logical AND | `&&` | left |
| Bitwise OR | `\|` | left |
| Bitwise XOR | `^` | left |
| Bitwise AND | `&` | left |
| Equality | `==`, `!=` | left |
| Relational | `<`, `<=`, `>`, `>=` | left |
| Shift | `<<`, `>>` | left |
| Additive | `+`, `-` | left |
| Multiplicative | `*`, `/`, `%` | left |
| Prefix | `-`, `~`, `!`, `&`, `*`, `++`, `--` | right |
| Postfix | call `()`, index `[]`, member `.`, `->`, `++`, `--` | left |

Parentheses group expressions; `(type)expression` requests a cast. Prefix
`&` takes an address; binary `&` performs bitwise AND. Unary `+` and `?:` are
unsupported. Updates evaluate the destination address once, load once, evaluate
the right operand, compute, convert back and store once. Compound updates wrap
to the destination width. Prefix returns the new value; postfix returns the
old value. Const/ROM/bool updates are errors. Volatile accesses remain distinct.

Assignment requires an l-value: a variable, dereference, indexed element, or
structure member. Its expression result is the assigned value. ROM and const
destinations are not writable; a mutable RAM local holding a ROM pointer is
itself writable. Address-of accepts variables, members, subscripts, and
dereferences, not arbitrary expressions.

### Numeric conversions

Byte operands widen to word preserving signedness for binary arithmetic,
bitwise operations, shifts, and comparisons. Mixing typed signed and unsigned
operands, or implicitly changing signedness in assignment/argument/return,
requires an explicit cast to the intended type. A literal paired with a typed
operand can adopt its signedness, so `unsigned_value + 1` stays ergonomic.
Comparison results are bool, not the promoted integer. Bool can widen to
integers as 0/1. Pointer conversions follow section 3.

Literals in declarations, arguments, assignments, and returns adopt the
expected type when possible and are range-checked. Without context, positive
0..127 infer signed byte, 128..255 unsigned byte, 256..32767 signed word, and
32768..65535 unsigned word. Unary-negative literals are checked as signed
values, including -128 and -32768. Literal representability is not arithmetic
wrapping: use an explicit cast to request truncation.

### Deterministic arithmetic

- Integers use two's-complement representation. Addition, subtraction,
  multiplication, and negation wrap modulo 2^N for signed and unsigned results.
  Signed overflow does not make a path unreachable.
- Narrowing requires an explicit cast. Casts retain the destination's low N
  bits and interpret those bits according to signedness; widening signed
  integers sign-extends, while widening unsigned integers zero-extends.
- Binary byte arithmetic promotes to 16 bits and therefore wraps at 16 bits.
  A byte cast wraps at 8 bits. Unary `-`/`~` keep the operand's integer width.
- Comparisons, `&&`/`||`, and `!` return bool. Conditions accept bool and
  integers, with zero=false and nonzero=true; pointers/aggregates are rejected.
- Bitwise operations act on the fixed-width bit patterns. `&&`/`||` evaluate
  their right operand only when necessary, unlike eager `&`/`|`.
- Shifts produce the promoted 16-bit left-operand type. Counts must be 0..15.
  A known invalid count is diagnosed; a dynamic invalid count stops with R6=5.
  Left shift retains low 16 bits. Signed right shift is arithmetic, unsigned
  right shift is logical. No count masking is implied. The count's signedness
  is independent of the left operand; this is not mixed-value arithmetic.
- Signed division truncates toward zero; remainder has the dividend's sign.
  -32768 / -1 wraps to -32768, with remainder zero. Known zero divisors are
  diagnosed; dynamic division/remainder by zero stops with R6=6.

GSU implements division/remainder with a bounded 16-step software core and
signed adjustment. Lack of an ISA DIV instruction does not remove a language
operator. Word multiplication uses the low 16 bits of a full-width product,
not the ISA's byte-only multiply.

### Evaluation order

Observable evaluations occur left to right: binary operands, call arguments,
and destination address before assignment value. Short-circuiting skips an
unneeded operand. Stack argument placement does not determine evaluation order.

IR lowers logical operators into conditional control flow with an aligned
temporary for the bool result. Loads/calls and potentially faulting
division/shifts execute at their definitions even when their result is unused.
Pure arithmetic may be recomputed from retained values; observable reads/calls
are not repeated.

## 6. Local storage, arrays, and structures

**Current.** Local declarations use `type name;` or `type name = expression;`.
Uninitialized locals are not automatically zeroed; reading one has no supported
value guarantee. Locals have automatic lifetime within their function call.
A pointer to a local must not be used after that call finishes. There is no
runtime lifetime or array-bounds checking.

One-dimensional arrays use a positive constant-expression size or infer it
from a nonempty initializer list/string:

```c
word values[4];
values[0] = 42;
```

Elements are contiguous, with stride equal to element storage size, not pointer
size. Arrays of pointers/structs, nested aggregate lists and struct array members
are supported. Missing entries become zero/null; excessive entries are errors.
Local entries may use runtime expressions, evaluated in declaration order.
Global entries must be constant expressions or checked literal/null pointers.
Multidimensional declarations and automatic local array decay are unsupported.
Pointer-plus-integer, pointer-minus-integer, and `pointer[index]` use the pointee
storage stride. Indexing through a pointer variable loads its stored address,
not the address of that variable. Multilevel near/far pointer indexing is
supported, including a four-byte stride for far-pointer elements. Integer-plus-
pointer and pointer differences are unsupported. Compatible pointers support
`==` and `!=`; mixed-reach comparisons require an explicit conversion.
Take `&values[0]` to obtain a local array's element pointer; ROM arrays decay to
ROM element pointers. Near accesses remain in the configured bank context.

Structures are named types, not anonymous C-compatible layouts:

```c
struct Packet {
    byte tag;
    word value;
    byte flags;
};

void main() {
    struct Packet packet;
    packet.tag = 1;
    packet.value = 42;
    packet.flags = 0;
}
```

**Current layout.** Members are laid out in declaration order. Each member starts
at an even offset; the total structure size is rounded to an even number of
bytes. `Packet` above has offsets 0, 2, 4 and total size 6. Local allocation is
also word-aligned; a contiguous byte array does not add padding between bytes.
`@packed` reduces member alignment to one. `@align(N)` requests a minimum
aggregate alignment, with N a power of two from 1 to 128. They can be combined.
Ordinary structs respect nested alignment. Local/static allocations and format
v6 RAM-section placement honor it, even from a word-aligned `--ram-origin`.
Packed layout does not legalize odd-offset word accesses: those members are
rejected on access/initialization. Packed byte-only structs work at odd strides.
Bit-fields and unions are unsupported. Use `pointer->member` or
`(*pointer).member` for a near or far structure pointer. Member addresses
retain the containing object's storage bank and are checked before access.
By-value recursive
structures are not supported; pointer references do not embed the structure.

**Current.** Structures are memory-resident aggregates. Member loads/stores,
arrays of structures, and passing a pointer are supported. The analyzer rejects
copy initialization from a struct value, whole-struct assignment, casts, parameters,
returns, and standalone value expressions, including unsupported prototypes.
Aggregate copying or a by-value/hidden-return-pointer ABI requires a separate
specified implementation.

## 7. Scopes, functions, and linkage

**Current.** Local declarations are visible after their initializer is analyzed.
A new block creates a lexical scope. Duplicate local names in one scope are
rejected; nested local shadowing is permitted. Every declaration has a stable
`SymbolId`; the backend must not resolve a reference again using its spelling.
Locals cannot shadow global function or data names. A `for` initializer
declaration is local to that loop's lexical scope.

Parameters are named and typed. Calls are to named functions, not dynamic
function-pointer expressions. Parameter count and types are checked; implicit
conversions follow section 5. All functions in the unit are registered before
body analysis, so a definition can be called before its textual position.

```c
word add(word left, word right);
void main() { word result = add(30, 12); }
```

A prototype has no body and emits no code. Repeated compatible prototypes are
allowed, conflicting signatures are rejected, and a function has at most one
definition per unit. A definition in another unit is resolved by the linker.
The object format does not currently enforce cross-unit parameter-type equality;
shared prototypes must agree. Unmarked functions and globals have external
linkage for compatibility. `export` makes this explicit. `internal` functions
and data have translation-unit-local identities and are not visible to other
objects, even when two objects reuse the same source name. Internal prototypes
require definitions in the same unit. `main` must be external.

`extern word shared;` declares RAM storage owned by another unit; it cannot have
an initializer. Repeated compatible extern declarations and one definition
can share a unit. Extern ROM objects are not implemented. Cross-unit data types
are not encoded/checked by the object format; shared declarations must agree.

Non-void functions must return a compatible value on all paths accepted by the
conservative return analysis. A void function may use `return;` or fall through.
No implicit numeric zero return is inserted. Recursion uses the configured
stack and has no compiler-proved maximum depth. GSU `main` ends with STOP rather
than returning to a SNES CPU caller; registers/local variables after STOP are
not a general public result API.

### Static RAM storage and startup

```c
word frame_counter;
byte sprite_buffer[128];
struct State game; // requires a preceding struct State definition
internal word calls = 0;
export const word limit = 149;
```

Globals have program-long lifetime. Uninitialized globals, aggregates and
padding are zeroed. Numeric constant expressions, initializer lists and checked
literal/null pointers are supported. Symbol-address relocations inside RAM
images are unsupported; initialize those pointers in code. Zero-initialized
pointers hold null, not dereferenceable addresses.

Objects carry a separate RAM initialization image. The linker allocates it in
the near RAM bank (`--ram-bank 0|1`) from an even `--ram-origin` offset
(default $0400), not in the ROM DATA payload. With `--init-runtime`, startup
clears the allocation and writes nonzero values before entering main, without
ROM reads. Initialization runs on each bootstrap entry; do not re-enter it if
retained state is required.

Linking globals without generated startup requires the explicit
`--host-initialized-globals` contract: the host owns matching RAMBR, stack, and
initial data setup. RAM storage must fit one bank and not overlap the payload;
with generated startup it must end below the initial stack pointer. Generated
stack guards stop before frames/temporaries descend into that allocation.
There is no independent BSS/exported loader table or dynamic-constructor ABI.

## 8. ROM constants

**Current.** Top-level ROM data uses these forms:

```c
rom const word answer = 42;
rom const unsigned byte palette[] = {0, 15, 255};
```

The element type is bool, byte or word. Scalars require one initializer. Array
extents can be constant expressions or inferred from the list or byte string;
explicit extents are zero-padded and must fit all supplied entries. Initializers must be typed scalar
constant expressions, including unary negatives, not runtime calls. Values are checked against the declared signedness/width; word
elements are serialized low byte first. Reads are runtime memory accesses, not
necessarily constant folding. Assignment is rejected.

**Current alignment.** Compiler DATA objects start at even offsets. Object
format versions 4 through 6 record DATA alignment; the linker pads after odd-length CODE
and between input DATA sections as needed. Word-array stride remains two bytes;
padding belongs between objects/sections, not between word elements. Raw
assembly defaults to packed alignment 1 and must request alignment 2 for typed
word data. See [object-format.md](object-format.md).

Linking currently puts all CODE before all DATA in one payload. A RAM-execution
build does not separately place ROM-qualified constants in ROM. A source type
qualifier is not a loader; applications must respect the loading limitations in
[gsu-loading.md](gsu-loading.md).

## 9. Control flow

**Current.** `if/else`, `while`, `for`, `switch/case/default`, `break`, and `return`
are available. Integer conditions are false when zero and true otherwise;
pointer/aggregate conditions are not part of the supported IR contract.
`continue` targets the nearest loop's increment block (`for`) or condition
(`while`). A switch does not change that target. `goto`, labels outside switch
and `do/while` are unsupported.

`for (initializer; condition; increment)` executes the initializer once, checks
the condition before each iteration, and executes the increment after the body.
An omitted condition is true. `break` leaves the nearest enclosing loop or
switch. It is an error outside both. Hardware-loop optimization must preserve
these source effects and must not erase diagnostics or observable loop values.

Switch labels accept typed integer constant expressions. Duplicate values or
multiple default labels are errors. The selector is integer. Cases fall through
in source order unless terminated by break/return/continue; no implicit break is
added. `fallthrough;` directly before the next label annotates this intent.
Unannotated nonempty fallthrough emits `-Wimplicit-fallthrough`.
Constant-selector dispatch is direct. The GSU backend uses a balanced comparison
tree for dynamic switches with at least four cases and a linear chain for
smaller ones. Dispatch strategy is not part of source semantics.

## 10. Plotting and target capabilities

**Current GSU syntax.** Plot contexts are lexical blocks:

```c
void main() {
    plot {
        set_color(3);
        plot(10, 20);
    }
    flush_pixels();
}
```

`plot { ... }` owns its context and local scope. Branches, loops, switches,
break, continue and return can occur inside it. Coordinates remain arbitrary
expressions. `plot(x, y)` draws a pixel. `plot.x`/`plot.y` explicitly expose R1/R2
inside the context. `plot_x` and `plot_y` are ordinary lexical identifiers.
Taking the address of a coordinate is rejected. Calls/division inside a
plot block preserve these registers. Nested contexts are rejected.
Context is per-instruction IR metadata, not a hardware resource requiring a
closing instruction on every exit.

`set_plot_options(value)` and `flush_pixels()` expose GSU drawing operations.
`draw at (x, y) with color value;` sets color then plots; the color clause is
optional, and the plot-context requirement still applies. `cache` is accepted
on function definitions and for/while loops, not variables or prototypes.
Neither syntax configures SNES screen memory, bus ownership, or cache timing.

Legacy `plot_begin;`/`plot_end;` pairs remain accepted for existing sources,
but must balance, cannot nest/cross loop boundaries inconsistently, and are
not allowed as boundaries inside switch. A legacy plot_end cannot close a
lexical block. New code should use lexical blocks.

Targets centrally declare graphics, instruction-cache, hardware-loop and far-data
capabilities. The analyzer and IR verifier enforce them. SPC700 rejects these
GSU operations with capability diagnostics. `--check` verifies common language
semantics on both targets; the SPC700 machine backend remains unimplemented.

## 11. Attributes and constant expressions

Attributes are uniformly parsed as `@name` or `@name(arguments)`. Unknown
handlers, duplicates and invalid placement are errors. Current handlers:

- `@cache`: GSU function definitions and for/while loops; legacy `cache`
  remains accepted. This requests cache instructions, not a timing guarantee.
- `@packed`, `@align(N)`: struct definitions (section 6).
- `@target(gsu)` / `@target(spc700)`: function assertions matching the CLI
  target, not target selection or conditional filtering.
- `@cfg(gsu)` / `@cfg(spc700)`: conditional top-level declarations and imports
  (section 16).

`@interrupt`, `@naked`, `@section`, `@bank`, `@calling_convention` and `@inline`
are reserved but unimplemented: every use is explicitly rejected. Their
[future contracts](inline-assembly-contract.md) do not enable entry/exit,
placement, inlining, or alternate-ABI behavior. This syntax provides extension
points without adding keywords for every backend.

```c
constexpr word TILE = 8;
const word WIDTH = 32;
enum Mode : unsigned byte { MODE_4BPP = 2, MODE_8BPP = 4 };
word buffer[WIDTH * TILE];
static_assert(sizeof(buffer) == 512);
```

`constexpr` declares a typed scalar integer/bool constant without storage.
Taking its address is illegal. Nonvolatile scalar `const` values with pure
initializers can also participate but still have storage. ROM constants are
storage, not a substitute for compile-time constants.

Enums have an optional byte/word signed/unsigned underlying type (default signed
word). Enumerators are unqualified compile-time names; omitted values increment
the preceding value, with range checking. `enum Name` uses the underlying
integer type, not a distinct nominal conversion domain.

The evaluator supports typed integer/bool operations, casts, short-circuit
logic, known constants and layout queries. It never executes calls or memory
accesses. Cyclic/deep dependencies and nonconstant required values are errors.
Define structs/enums in dependency order; arbitrary forward layout resolution
is unsupported.

`sizeof(type)` / `sizeof(expression)` report storage bytes; `alignof` reports
alignment. Expression operands are type-checked but unevaluated, including
volatile accesses. `offsetof(struct Type, member)` reports a direct member
offset. Results are unsigned words. Incomplete/void types, unknown members and
values exceeding 65535 are errors. `static_assert(expression);` requires a
nonzero constant expression; message arguments are unsupported.

## 12. Character and string literals

Raw quoted text is ASCII. Character literals contain exactly one decoded byte.
Strings initialize byte arrays in RAM or ROM and append one NUL byte. Explicit
extents must include that terminator; omitted extents infer the full decoded
length. Missing remaining entries are zero initialized.

Escapes are `\n`, `\r`, `\t`, `\0`, `\\`, `\'`, `\"`, and
`\xNN` with exactly two hexadecimal digits. Hex escapes permit non-ASCII
bytes. Raw non-ASCII, invalid escapes, multiline/unterminated quotes and
multibyte character literals are rejected. Decoded literals are bounded to
65534 bytes. Strings are not runtime pointer expressions; no Unicode, interning,
concatenation or automatic string-to-pointer decay is implied.

```c
rom const byte name[] = "PLAYER"; // seven bytes including NUL
unsigned byte raw[] = "\xFF\0";  // FF, 00, 00
```

## 13. Modules and interfaces

```c
module Main;
import "math.dc";
void main() { word value = add(46, 103); }
```

A module header is optional on `.dc` units and mandatory on `.dci` interfaces.
Imports appear only at file scope, before declarations, and name quoted relative
`.dc` source or `.dci` interface paths. `@import` and logical imports such as
`import math;` are not supported; `@...` remains attribute syntax. The optional
source module name does not change filename resolution or add a namespace.

A source import makes public declarations available, not the file's text.
Functions become prototypes and RAM/ROM objects become external references;
function bodies and storage initializers are never duplicated in an importer.
Types, aliases, enums and compile-time constants are available to the importer.
Resolved public constants/layouts may depend on private constants without making
those private names visible. Symbols are public by default; `export` is explicit
public linkage, and `internal` functions/data/constants stay private.

```c
// math.dc
internal word helper(word x) { return x + 1; }
export word add(word a, word b) { return a + b; }

// main.dc
import "math.dc";
void main() { add(1, 2); } // helper(1) would be an undeclared-symbol error
```

Imports can themselves import files. A build-owned dependency graph parses each
physical file once, deduplicates repeated/shared imports and path aliases, and
checks dependencies before importers. Cycles report their file chain and import
location; missing files and malformed imported sources are errors. Definitions
are emitted only by their own compilation unit. Names remain unqualified, so
incompatible declarations from different imports conflict rather than gaining
implicit namespaces. Distinct `.dci` interfaces cannot reuse a module name.

Resolve a path relative to the importing file first, then search configured
directories in order. The first existing file wins; a malformed local file does
not trigger fallback. `[compiler].import_paths` in `discoc.toml` supplies up to
64 manifest-relative directories. Explicit repeatable `--import-path DIR`,
`-I DIR` or `-IDIR` replaces that list with CLI-relative directories. See
[project-manifest.md](project-manifest.md#source-dependencies-and-import-paths).

Declaration-only `.dci` interfaces remain supported. They can import sources or
interfaces and contain structs, enums, type aliases, constexpr, static assertions,
public prototypes and extern RAM declarations, not bodies/storage definitions.
Importing an interface does not infer a matching implementation filename.

The loader reads imports before parsing declarations, making imported aliases
available in types, casts and attributes. Transitive aliases from a shared
file are deduplicated by declaration identity; separately declared aliases
with the same name conflict even if their underlying types match. `@cfg` can
select imports without opening an inactive path (section 16).

`discc build` discovers and compiles transitive `.dc` imports once into separate
objects, without requiring every dependency in `project.sources`. Ordinary
`discc main.dc -o main.o` checks the reachable graph but emits only that unit;
compile/link dependencies separately or use project build mode. `.dci` imports
never automatically compile a matching `.dc`. See the
[source-import example](../examples/source_imports/README.md) and
[interface workflow](../examples/language_modules/README.md).

Limits are 32 import levels, 128 total files, 128 imports per file, 16 MiB per
file, 32 MiB total source and 4,096 bytes per import path. Imported diagnostics
retain the originating source/interface path. The cache is per invocation,
not persistent or incremental. There is no package resolver or C preprocessor.

## 14. Warnings and conformance

Default categories are `shadowing`, `unused-variable`, `unused-function`
(internal definitions), `uninitialized`, `unreachable` and
`implicit-fallthrough`. `-Wall` additionally enables `expensive-helper`
for nonconstant software division/remainder. `-W<category>` enables,
`-Wno-<category>` disables, and `-Werror` makes enabled warnings fatal.
Warnings are bounded to 256 and never change generated code. Signedness
mixing and implicit narrowing remain errors, not suppressible warnings.

Definite assignment is conservative for scalar locals and branches/loops. It
is not field-sensitive, interprocedural, or proof about aliases. Unused public
functions are not diagnosed. Cost warnings identify software sequences, not
measured cycle estimates.

`tests/language/` is separate target-independent conformance: positive and
negative cases by rule, checked with `discc --check` on GSU/SPC700 except
explicit capability cases. Frontend unit, IR verifier, malformed-object and
backend execution tests remain independent. Execution groups compare direct,
assembled and mixed-object payloads byte for byte, then run the GSU instruction
model to verify aligned layouts, single-evaluation updates, continue, arrays,
strings, null, volatile effects and imported interfaces. Graphics coverage checks
emission, not rendered pixels.

Parser nesting is limited to 128 entries and expression trees to depth 256.
Aggregate-by-value ABI, interbank code calls, named far placement, explicit
`extern rom` syntax, complete WLA-DX export and the SPC700 backend remain future
work; importing public ROM data already creates checked external references.

## 15. Transparent type aliases

**Current.** The builtin aliases preserve `byte`/`word` and their exact rules:

| Alias | Existing type |
| --- | --- |
| `u8` | `unsigned byte` |
| `i8` | `byte` |
| `u16` | `unsigned word` |
| `i16` | `word` |

Define a top-level alias with `type Name = type;`:

```c
type Counter = u16;
type Status = const volatile u8*;
type Remote = far i16*;
type RemoteSlot = Remote*; // near pointer to a four-byte far pointer
struct Pair { i16 left; i16 right; };
type Pair = struct Pair;
```

Aliases are owned copies of canonical type metadata, not new nominal types or
conversion domains. Scalar, void, enum, struct and pointer types can be aliased;
array/function aliases and local aliases are unsupported. An alias must be
declared or imported before it is used; unknown/forward/cyclic aliases are
diagnosed, not recursively expanded without bounds. Struct/enum definitions
still follow their existing dependency-order rules.

Signedness is fixed by the alias; `unsigned i16` is an error. Const/volatile can
be added to the aliased object without discarding existing pointee qualifiers.
A pointer alias fixes its address space and retains every inner reach/qualifier.
Adding `*` creates a new near outer layer; an explicit `far` qualifies the outer
layer as usual. Scalar aliases can use an explicit ROM/RAM storage qualifier.
All existing alignment/conversion/ABI checks still apply.

Alias names cannot be reused as function, parameter, variable or enumerator
names, including conflicts discovered after parsing. Struct tags remain
explicit (`struct Pair`), so a same-spelled convenience alias is allowed.
The visible alias-table limit is 4096 bindings including the four builtins;
pointer depth stays limited to 32, including depth built through aliases.

## 16. Conditional target declarations

**Current.** `@cfg(gsu)` or `@cfg(spc700)` selects top-level declarations or
imports for the CLI target. It takes exactly one bare supported target name.
It is not accepted on local statements/loops and does not implement macros,
conditional textual expansion, arbitrary predicates, or CLI-defined symbols.

```c
module Device;
@cfg(gsu) import "gsu.dci";
@cfg(spc700) import "spc700.dci";

@cfg(gsu) @target(gsu) i16 selected() { return 149; }
@cfg(spc700) @target(spc700) i16 selected() { return 42; }
void main() { i16 value = selected(); }
```

Inactive declarations are parsed but not registered, analyzed, lowered or
emitted. Syntax and attribute validity must still hold; type aliases needed
to parse an inactive declaration must still be visible. An inactive alias
does not enter the type table. Target-selected alternatives may reuse a name
because only one declaration is active. Inactive function bodies do not need
to resolve target-specific calls or satisfy the other target's capabilities.

Inactive imports are not opened and contribute no names, but must still use
valid relative `.dc`/`.dci` path syntax. They count toward the bounded import count.
`@target` remains an assertion on an active function, not a selector; `@cfg`
does not silently suppress unknown/reserved attributes or malformed target
names. Actual implementations remain separately compiled and linked.

## 17. Library and future systems features

**Current.** `lib/core` provides signed Q8.8/Q12.4 fixed-point functions and
near-RAM byte copy/move/fill. Fixed-point aliases do not introduce primitives
or automatic scaling. `lib/targets/gsu` provides graphics wrappers using the
GSU capability operations. See [freestanding-library.md](freestanding-library.md)
for exact rounding, wrapping, fault, memory and linking contracts.

**Planned.** Inline ASM requires typed inputs/outputs, clobbers, target state
and memory effects before it can enter verified IR; an opaque text-only ASM
escape hatch is not supported. ABI-oriented attributes remain reserved until
entry/exit and placement contracts exist. Q16.16 needs a 32-bit value ABI;
BF16/FP16 is lower priority than the integer/fixed-point foundations.
Function pointers, unions and aggregate-by-value ABIs remain deferred.
Classes, exceptions, generics, automatic heap allocation, variadic functions
and ISO C/preprocessor compatibility are not goals of this refactoring.
