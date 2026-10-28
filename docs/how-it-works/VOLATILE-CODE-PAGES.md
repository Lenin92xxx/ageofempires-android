# Caching volatile self-rewriting code (patch 0020)

**Result (Age of Empires II: DE, Honor Magic V2, Snapdragon 8 Gen 2, GameNative):** a single-player match that took
about **7 to 10 minutes** to load on `aoe4-perf5-23` (the v1.6.0 patch set, patch 0017) loads in about **1.5 to 2
minutes** with this patch on top, and holds a stable 40 FPS at a 40 cap with the main thread about half idle. The
mechanism-level counters below come from an in-process stats log (patches 0018 and 0019, off by default); match-load
times are wall-clock, FPS is the game's own frame cap. Measured on 2026-10-27/28 across several sessions on the phone,
not with the Thor benchmark — the numbers want re-measuring on the reference rig.

This builds directly on patch **0012** (volatile code regions) and **0014** (reuse of re-decrypted translations), and
is the same idea pushed further: 0012 stops write-trapping the 16 MB one-instruction buffer and shares
`[instruction][jmp]` slots by content; this patch extends content sharing to the cases 0012 left recompiling on every
visit, applies it to the image pages the protection rewrites, and removes the per-visit lock those visits were taking.

## What was still slow after 0012/0014

With the v1.6.0 set, the stats log on a loading match showed, on the main thread and the protection's own threads:

- **~6,700 SMC write faults per second**, 95 % of them on a single image page (`AoE2DE_s.exe+0xf77000`). 0012 handles
  the *private* 16 MB buffer, but the protection also rewrites code in its own image pages, which stayed write-trapped:
  every rewrite faulted, invalidated and recompiled.
- **~13,500 `NtProtectVirtualMemory` and ~27,000 `NtQueryVirtualMemory` calls per second** from the protection.
- **~20,000 `c000001d` (illegal instruction) exceptions per second** at four fixed trap sites plus a per-instruction
  stepper.
- Every SMC fault took the `CodeInvalidationMutex` **exclusively**. While one thread invalidated, the others blocked in
  `CompileBlock`; the `PhoenixTaskManagerThread`s spent ~160 s each waiting during a load.
- Inside the stepper, the blocks 0012 could not share were recompiled on every visit: the four trap sites
  (~24,000/s — an invalid instruction decodes to size 0, so they fell out of the content cache), the
  `[instruction][jmp]` slots whose jmp returns into the protection's own (volatile) code (~24,000/s, excluded only
  because the target was volatile), and lone `ret` slots (~12,000/s, not the `[instruction][jmp]` shape at all).

0014 already keeps translations of re-decrypted code, but these volatile blocks are deliberately never put in the
lookup cache, so 0014 does not cover them.

## What the patch does

**Volatile image pages.** An image page that takes 16 write faults within one second is marked a *volatile code page*
(like 0012's private regions, `FEX_EXP_VOLPAGE=0` to disable). It is no longer write-trapped; its blocks are compiled
as ordinary multiblocks (loops stay inside one block) and cached by guest RIP plus a hash of the bytes of every
sub-block, re-checked on every entry against the page's current contents and the thread's code buffer. SMC faults on
these pages fell from ~6,700/s to **~170/s** and `NtProtect` calls from ~13,500/s to **~400/s**.

**Trap sites cached by their real window.** A trap the protection places (an instruction FEX cannot decode) ends a
block with decode size 0. The decoder now records how many bytes it examined before rejecting the instruction
(`FailExaminedSize`), and the content key uses exactly those bytes, only when the trap is the first instruction of the
entry block. The bytes *after* the trap — which can change between visits while the trap itself does not — are left out
of the key. The four trap sites stopped recompiling (~24,000/s → a handful).

**`[instruction][jmp]` with a volatile target shared.** 0012 shared these only when the jmp target was non-volatile.
A target in volatile code is now allowed: an exit to volatile code is never backpatched (`ExitFunctionLink` returns
without patching the stub for a volatile `GuestRIP`), so the block re-resolves its target through the dispatcher on
every execution — exactly what a fresh compile would do. The host code is identical wherever the slot sits, so it is
keyed by `(instruction bytes, absolute target)`.

**Lone `ret` / indirect `jmp` shared by content.** A one-instruction block whose exit target is only known at run
time — `ret`, `ret imm16`, or `jmp r/m` without a RIP-relative operand — produces host code that reads nothing of its
own address (`RETOp` pops the target and exits through the call-ret stack or the dispatcher; `JUMPAbsoluteOp` exits
through the dispatcher). Every copy of the same bytes shares one translation, keyed by the bytes and a dynamic-target
sentinel. Recognised by the dispatch-handler pointer, not the raw opcode. (`call` indirect is excluded: it pushes a
return address that depends on the slot's location.) In the stepper this collapsed ~12,000 recompiles/s to a couple
hundred.

**A per-thread lock-free cache.** Volatile code is never in the lookup cache and is re-validated against the guest
bytes on every visit, so the `CodeInvalidationMutex` protects nothing for it. A small direct-mapped per-thread cache
sits in front of the shared one, checked at the top of `CompileBlock` before the mutex. An entry is valid only while
the thread's code buffer, an executable-range epoch (bumped whenever any memory stops being executable), and the byte
hash all still match. In a loading match it served ~73 % of volatile visits (~135,000/s) with no lock taken.

### Correctness guards

- Not shared under the trap flag (single-step produces different code) or under `SMCChecks=full` (full SMC checks embed
  each instruction's guest address).
- A translation only enters the content cache if the decode it was compiled from still hashes to the decode the key was
  taken from. If another thread rewrites the slot in between, the code is correct for that one visit but is not cached
  under a key it no longer matches.
- Exits to volatile targets are never backpatched (relied on above), so a shared block never holds a stale pointer into
  a volatile target's old translation.

## Diagnostics (patches 0018 and 0019)

The measurements above come from two diagnostic patches, both off unless `FEX_EXP_STATSLOG` is set, which should not be
part of a shipping build but are included because every number here was read from them:

- **0018** writes a per-second log (`FEX_EXP_STATSLOG=<folder>`, `1` = the Download folder; `FEX_EXP_STATSHZ` sets the
  sampling rate): per-thread CPU, JIT time and count, SMC faults, exception time, and a sampled RIP histogram of the
  busiest threads (suspend + `NtGetContextThread`, as a profiler does).
- **0019** adds the observers the log reads: SMC-fault pages with the writing RIP, volatile compiles, disk-cache
  reloads, and guest exceptions by code and site.

## Upstream

The volatile-code-region idea (0012/0014) is FEX issue
[#6022](https://github.com/FEX-Emu/FEX/issues/6022). This patch is an extension of the same approach and would belong
in the same discussion before a clean FEXCore PR; nothing in it is specific to this game beyond the heuristic that
promotes a frequently-rewritten page to a volatile one.

## Build

Applied after the v1.6.0 set, in order: 0004, 0006, 0007, 0009, 0010, 0012, 0013, 0014, 0015, 0016, 0017, then 0018,
0019, 0020, then `ninja arm64ecfex`. On a clean `7d3090f` the whole series reproduces the tested DLL except the 4
build-timestamp bytes (PE header and debug directory).
