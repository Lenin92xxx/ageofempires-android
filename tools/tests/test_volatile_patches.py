#!/usr/bin/env python3
"""Run host regression checks against code from an applied FEX patch series.

Usage: python3 tools/tests/test_volatile_patches.py /path/to/patched/FEX

The harness compiles the real source fragments, not copies of their logic.
It checks memory boundaries, decoder policy, and diagnostic keys. It does not
replace Windows/FEX execution tests or a game performance test.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def harness(tree):
    core = (tree / "FEXCore/Source/Interface/Core/Core.cpp").read_text()
    frontend = (tree / "FEXCore/Source/Interface/Core/Frontend.cpp").read_text()
    module = (tree / "Source/Windows/ARM64EC/Module.cpp").read_text()
    sample_start = core.index("uint64_t Sample = 0;", core.index("VolatileCodeStats.ShapeSampleIdx"))
    sample_end = core.index("VolatileCodeStats.ShapeSample[", sample_start)
    sample = core[sample_start:sample_end]
    guard_start = frontend.index("// Patch 0020: code in a volatile page")
    guard_end = frontend.index("LastFieldReadSize = 0;", guard_start)
    guard = frontend[guard_start:guard_end]
    exc_start = re.search(r"(?:struct ExceptionStat\s*\{|RipStat ExcTable\[)", module).start()
    exc_end = module.index("void OnDiskCacheHit(", exc_start)
    exc = module[exc_start:exc_end]
    exc_format = re.search(r'Out \+= fextl::fmt::format\([^\n]*Describe\(Exc\[i\][^\n]*;', module).group()
    rip_struct = re.search(r"struct RipStat \{.*?\n\};", module, re.S).group()
    return r'''
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
namespace fextl { using string = std::string; template<class T> using vector = std::vector<T>; namespace fmt = ::fmt; }
using WCHAR = char16_t;
struct UNICODE_STRING { uint16_t Length; uint16_t MaximumLength; WCHAR* Buffer; };
struct MEMORY_BASIC_INFORMATION { void* BaseAddress; void* AllocationBase; size_t RegionSize; unsigned Type; unsigned State; };
struct IMAGE_DOS_HEADER { int32_t e_lfanew; };
struct IMAGE_NT_HEADERS64 { struct { uint32_t SizeOfImage; } OptionalHeader; };
constexpr unsigned MEM_IMAGE = 0x1000000, MEM_FREE = 0x10000;
constexpr int MemoryBasicInformation = 0, MemoryMappedFilenameInformation = 2;
static uint8_t* Image;
static size_t Page;
void* NtCurrentProcess() { return nullptr; }
int NtQueryVirtualMemoryNative(void*, void* address, int kind, void* output, size_t, void*) {
  if (kind != MemoryBasicInformation) return -1;
  auto p = reinterpret_cast<uintptr_t>(address), base = reinterpret_cast<uintptr_t>(Image);
  if (p < base || p >= base + 2 * Page) return -1;
  auto* info = static_cast<MEMORY_BASIC_INFORMATION*>(output);
  info->AllocationBase = Image;
  info->BaseAddress = Image + (p >= base + Page ? Page : 0);
  info->RegionSize = Page;
  info->Type = MEM_IMAGE;
  info->State = 0x1000;
  return 0;
}
struct ModuleRec { uint64_t Base, End; fextl::string Name; };
fextl::vector<ModuleRec> Modules;
''' + function(module, "const ModuleRec* FindModule(") + r'''
struct TestBlock { uint64_t Size; uint64_t NumInstructions; bool ForceFullSMCDetection; };
__attribute__((noinline)) uint64_t Sample(uint64_t GuestRIP, uint64_t size) {
  const TestBlock Block {size, 0, false};
''' + sample + r'''
  return Sample;
}
struct TestContext {
  bool IsVolatileCode(uint64_t page) const { return page == 0x1000; }
};
bool DecodeNext(uint64_t address, uint64_t& CurrentCodePage, TestBlock& block, bool StopAtVolatileCode = true) {
  TestContext context;
  auto* CTX = &context;
  auto* BlockIt = &block;
  bool EntryBlock = true;
  uint64_t OpMinPage = address & ~uint64_t{4095};
  uint64_t OpMaxPage = (address + 15) & ~uint64_t{4095};
  struct { std::set<uint64_t> CodePages; } BlockInfo;
  for (;;) {
''' + guard + r'''
    return true;
  }
  return false;
}
''' + rip_struct + r'''
constexpr size_t RipTableSize = 8192;
''' + function(module, "size_t HashSlot(") + "\n" + function(module, "void AddRip(") + "\n" + exc + r'''
fextl::string Describe(uint64_t) { return "site"; }
fextl::string ExceptionOutput() {
  using Entry = std::remove_reference_t<decltype(ExcTable[0])>;
  fextl::vector<Entry> Exc;
  for (const auto& e : ExcTable) if (e.Count) Exc.push_back(e);
  fextl::string Out;
  for (size_t i = 0; i < Exc.size(); ++i) {
''' + exc_format + r'''
  }
  return Out;
}
int main(int argc, char** argv) {
  struct rlimit limit {0, 0};
  setrlimit(RLIMIT_CORE, &limit);
  assert(argc == 2);
  Page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  Image = static_cast<uint8_t*>(mmap(nullptr, 2 * Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  assert(Image != MAP_FAILED);
  const std::string mode = argv[1];
  if (mode == "sample") {
    auto* rip = Image + Page - 2;
    rip[0] = 0xff; rip[1] = 0xd0;
    assert(mprotect(Image + Page, Page, PROT_NONE) == 0);
    assert(Sample(reinterpret_cast<uintptr_t>(rip), 2) == 0xd0ff);
    assert(Sample(reinterpret_cast<uintptr_t>(Image + Page), 0) == 0);
  } else if (mode == "boundary") {
    uint64_t current = 0;
    TestBlock block {0, 0, false};
    assert(DecodeNext(0xffe, current, block));
    assert(block.ForceFullSMCDetection);
    ++block.NumInstructions;
    assert(!DecodeNext(0x1003, current, block));
    current = 0x1000;
    assert(!DecodeNext(0x1004, current, block));
    TestBlock ordinary {0, 0, false};
    current = 0x2000;
    assert(DecodeNext(0x2100, current, ordinary));
    assert(!ordinary.ForceFullSMCDetection);
    TestBlock vol {0, 0, false};
    assert(DecodeNext(0x1100, current, vol, false));
    assert(!vol.ForceFullSMCDetection);
  } else if (mode == "module") {
    assert(mprotect(Image, Page, PROT_NONE) == 0);
    const auto* module = FindModule(reinterpret_cast<uintptr_t>(Image + Page + 8));
    assert(module && module->Base == reinterpret_cast<uintptr_t>(Image));
    assert(module->End == reinterpret_cast<uintptr_t>(Image + 2 * Page));
    assert(module->Name == "image");
  } else if (mode == "exceptions") {
    OnGuestException(0xc0000005, 0x1234, true);
    OnGuestException(0x40000005, 0x1234, false);
    OnGuestException(0xc0000005, 0x1234, true);
    OnGuestException(0x80000003, 0x1234, true);
    size_t entries = 0;
    for (const auto& e : ExcTable) entries += e.Count != 0;
    assert(entries == 3 && ExcCount == 4 && ExcSlow == 1);
    const auto output = ExceptionOutput();
    assert(output.find("c0000005@site:2") != std::string::npos);
    assert(output.find("40000005@site:1") != std::string::npos);
    assert(output.find("80000003@site:1") != std::string::npos);
  } else return 2;
  munmap(Image, 2 * Page);
}
'''


class RegressionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="fex-volatile-tests-")
        folder = Path(cls.temp.name)
        source = folder / "regressions.cpp"
        source.write_text(harness(TREE))
        cls.binary = folder / "regressions"
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++")
        if not compiler:
            raise RuntimeError("A host C++20 compiler is required")
        subprocess.run([compiler, "-std=c++20", "-O2", "-pthread", "-I", str(TREE / "External/fmt/include"),
                        str(source), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check_case(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr or f"{mode} failed with status {result.returncode}")

    def test_diagnostic_sample_respects_decoded_size(self):
        self.check_case("sample")

    def test_cross_page_entry_checks_smc_and_stops_at_volatile_code(self):
        self.check_case("boundary")

    def test_module_lookup_does_not_read_inaccessible_headers(self):
        self.check_case("module")

    def test_exception_keys_and_output_preserve_complete_status(self):
        self.check_case("exceptions")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fex", type=Path)
    args = parser.parse_args()
    TREE = args.fex.resolve()
    unittest.main(argv=[__file__], verbosity=2)
