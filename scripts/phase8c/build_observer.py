#!/usr/bin/env python3
"""Build an isolated observer from a preserved source root and matching library.

Usage: build_observer.py SOURCE_ROOT LIBRWE_A OUTPUT_DIR [FROZEN_REFERENCE_ROOT]
Uses the current optimized build's compiler/flags/dependencies. Never edits inputs.
The source root must match the library's ABI. The current benchmark harness is
used for both revisions; an optional frozen reference is renamed and checked
after every measured tick when RWE_BENCH_HASH_TICKS is enabled.
Keep performance and diagnostic
runs separate: this binary replaces C++ new for allocation accounting.
"""
import json, pathlib, shlex, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[2]
src,lib,out=map(lambda s:pathlib.Path(s).resolve(),sys.argv[1:4])
reference=pathlib.Path(sys.argv[4]).resolve() if len(sys.argv)>4 else None
out.mkdir(parents=True,exist_ok=False)
build=root/'build-prof'
flags={}
for line in (build/'CMakeFiles/rwe_bench.dir/flags.make').read_text().splitlines():
    if line.startswith('CXX_') and ' = ' in line:
        k,v=line.split(' = ',1); flags[k]=shlex.split(v)
args=flags['CXX_DEFINES']+flags['CXX_INCLUDES']+flags['CXX_FLAGS']
args=['-I'+str(src/'src')]+args
hashcode=(src/'src/rwe/sim/GameHash_util.cpp').read_text().replace('"GameHash_util.h"','<rwe/sim/GameHash_util.h>')
hashcode+='\n#include "'+str(root/'scripts/phase8c/hash_diagnostics.inc')+'"\n'
(out/'hash.cpp').write_text(hashcode)
# Identical current observer harness for each matching source/library pair.
bench=(root/'src/rwe_bench.cpp').read_text()
bench=bench.replace('int main(int argc, char* argv[])','namespace rwe { void phase8cHashDiagnostics(const GameSimulation&, unsigned); }\nint main(int argc, char* argv[])')
bench=bench.replace('auto t0 = std::chrono::steady_clock::now();\n            GameHash acc(0);','phase8cHashDiagnostics(sim, iters);\n            auto t0 = std::chrono::steady_clock::now();\n            GameHash acc(0);')
# Optional post-scenario breakdown, outside all tick timing intervals.
bench=bench.replace('const auto& pt = sim.tickPhaseTimings;','if (std::getenv("RWE_HASH_DIAGNOSTICS")) { phase8cHashDiagnostics(sim, 1000); }\n            const auto& pt = sim.tickPhaseTimings;')
bench='namespace rwe { class GameSimulation; void phase8cHashDiagnostics(const GameSimulation&, unsigned); }\n'+bench
bench+='\n#include "'+str(root/'scripts/phase8c/allocations.inc')+'"\n'
if reference:
    # Independent frozen contract implementation with distinct symbols. This
    # validation build is not used for performance acceptance.
    import re
    def rename(text):
        return re.sub(r'\b(computeHashOf|combineHashes|mixGameHash|hashFeatureIdentities|SimulationHashContractVersion)\b',lambda m:'reference_'+m[0],text)
    (out/'reference.h').write_text(rename((reference/'src/rwe/sim/GameHash_util.h').read_text()))
    code=rename((reference/'src/rwe/sim/GameHash_util.cpp').read_text()).replace('"GameHash_util.h"','"reference.h"')
    (out/'reference.cpp').write_text(code)
    pos=bench.index('namespace rwe\n{')
    bench=bench[:pos]+'namespace rwe { GameHash reference_computeHashOf(const GameSimulation&); }\n'+bench[pos:]
    bench=bench.replace('const auto hash=sim.computeHash();','const auto hash=sim.computeHash(); if(hash!=reference_computeHashOf(sim)) { die("Phase8B reference hash mismatch"); }')
    bench=bench.replace('hash_tick_checksum=','reference_checked_hash_tick_checksum=')
(out/'bench.cpp').write_text(bench)
commands=[]
for name in (['hash','bench','reference'] if reference else ['hash','bench']):
    cmd=['/usr/bin/c++',*args,'-c',str(out/(name+'.cpp')),'-o',str(out/(name+'.o'))]
    commands.append(cmd);subprocess.run(cmd,check=True,cwd=build)
link=shlex.split((build/'CMakeFiles/rwe_bench.dir/link.txt').read_text())
link=[str(out/'bench.o') if a=='CMakeFiles/rwe_bench.dir/src/rwe_bench.cpp.o' else str(lib) if a=='librwe.a' else a for a in link]
link[link.index('-o')+1]=str(out/'rwe_bench')
link.insert(link.index(str(lib)),str(out/'hash.o'))
if reference: link.insert(link.index(str(lib)),str(out/'reference.o'))
commands.append(link);subprocess.run(link,check=True,cwd=build)
(out/'commands.json').write_text(json.dumps(commands,indent=2))
