# image-scan

Reading the game's own code, at runtime, because there is no other way.

Not a plugin — a static library, for the same reason
[`chat-sdk`](../chat-sdk) is one: it hands back `std::string` and
`std::vector`, and publishing those across two independently built DLLs with
their own static CRTs is undefined the moment one is rebuilt with a different
toolset.

It only ever **reads** this process's own memory. Nothing here patches or hooks
anything.

## Why it exists

**D2R.exe ships with an encrypted `.text`.** Measured over 4 MB of it:

| | D2R.exe `.text` | ordinary MSVC x64 code |
|---|---|---|
| Shannon entropy | **8.000 bits/byte** | ~6.2 |
| zero bytes | 0.39% (= 1/256) | several % |
| `0xCC` padding | 0.39% (= 1/256) | several % |

That is a uniform distribution, which is to say noise. The consequences are
worth stating plainly, because each one *looks* like an ordinary negative
result:

- A disassembler desynchronises at the first byte and then prints plausible
  instructions that are nonsense. `dumpbin /DISASM` renders D2R.exe's first
  instruction as `test bh,al`.
- A byte scan for `lea reg,[rip+disp32]` — which needs no disassembler — finds
  **zero** references into `.rdata` or `.data`, across the whole 22 MB section.
- There are no relocated pointers to those strings either.

What is *not* encrypted is `.rdata`, `.pdata` and the section table. So string
literals are readable on disk, and `.pdata` — the exception directory, a sorted
table of `RUNTIME_FUNCTION { BeginAddress, EndAddress, UnwindData }` with
105,655 entries — gives exact function bounds for any address.

And the code is readable **in memory**, after the loader has decrypted it. A
plugin runs inside the game process, so it is the one place the analysis can
happen at all.

## What it does

```cpp
#include <D2RLImage/image_scan.h>

// Find who loads a string, and the function that does it.
D2RLImage::ReportReferences(
    context, "my-plugin: ", context->exeBase,
    "Could not find ExecuteCommandCallback: %s",
    ".rdata", ".text", 8, /*dumpFunctions=*/true);
```

| Function | What it gives you |
|---|---|
| `ReadSections` | the section table of a loaded module |
| `FindString` | every rva of a null-terminated literal in a section |
| `FindLeaReferences` | every `lea reg,[rip+disp32]` that resolves to an rva |
| `FunctionRange` | `.pdata` bounds of the function containing an rva |
| `DumpRegion` | a function's bytes as hex, for offline disassembly |
| `ReportReferences` | all of the above, for one string, into the log |

`FindLeaReferences` decodes nothing but the one instruction form it is looking
for — `48/4C 8D <modrm mod=00 rm=101> disp32`, seven bytes, reaching
`rva(next instruction) + disp32`. That is deliberate: when the surrounding bytes
cannot be trusted to disassemble, matching a single fixed encoding is the only
thing that stays correct.

## Disassembling what it dumps

`DumpRegion` writes hex, one function per run of lines. Turn it back into bytes
and point a real disassembler at it:

```bash
objdump -D -b binary -m i386:x86-64 --adjust-vma=0x1713A90 function.bin
```

CLion ships a MinGW `objdump` if there is not one on `PATH`.

## Cost, and why it is a console command

Scanning is not free: `.text` is 22 MB and `.rdata` is 3.8 MB, so a scan for one
string reads about 26 MB. That is fine for a diagnostic run on demand and
completely wrong for anything on a frame path.

The one thing to get right is readability checking. Asking the OS whether each
byte is mapped is a `VirtualQuery` **per byte** — 22 million syscalls per
scanned string, which does not just take a while, it stalls the thread inside a
live game. `VirtualQuery` reports the size of the region it describes, so the
library walks regions once, builds the list of readable runs, and then scans
them as plain memory.

## Prior art, and a debt

[`plugins/hardcore-only/src/image_scan.cpp`](../../plugins/hardcore-only/src/image_scan.cpp)
did all of this first — it is where the encrypted-`.text` finding was originally
written down — and still carries its own copy of the machinery. It should move
onto this library; two copies of a memory scanner is exactly the drift
`chat-sdk`'s README warns about.
