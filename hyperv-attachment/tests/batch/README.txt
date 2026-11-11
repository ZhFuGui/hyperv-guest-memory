Load the dependency environment from BUILDING.md, then run:
  .\hyperv-attachment\tests\batch\run.ps1
Install requirements.txt into the Python environment if required.

The project compiles the CURRENT production batch handler, hypercall dispatcher,
vCPU lifecycle, memory copy/walks and usermode wrapper. Include aliases replace
only hardware context and the physical identity map; no guest code runs on the
host. The production MASM stub is linked separately and executed in Unicorn.
map-bounds.vcxproj additionally compiles the REAL production identity mapper
without aliases and checks returned pointers without dereferencing them. This
guards against a mock accidentally rejecting addresses that production accepts.

Coverage: 64th bitmap bit; zero-byte item; 64 KiB cap and invalid total; full-width
count; malformed/overflowing addresses; control/output overlap; unmapped and
partially mapped inputs/outputs; split control buffers; source and destination
pages with nonidentity GPA/HPA mappings; request snapshot; fixed SLAT context;
all-failed publication; vCPU reentry; 8 CPUs x 300 batches; randomized payloads;
startup allocation failure/move ownership; actual operation 11 dispatch; SDK
argument marshalling and strict boolean result; RBX/unwind/64-bit RAX in MASM.
Review regressions also cover CPU indices beyond the initialized count, zero
hardware roots, roots/tables/leaves beyond the 512 GiB identity slot, physical
copy overflow, and 4 KiB / 2 MiB / 1 GiB walks with entry snapshots.

The suite also compiles the current virt_find_list (operation 12): empty list,
first/last hit, miss, 16-byte exact key, nonzero/max window, default 1024 and
maximum 4096-node budgets, stateless continuation, invalidated cursors, self and
detached cycles, split source/control/result pages, malformed offsets and
noncanonical containers, candidate mutation, output clearing, reentry,
publication failure, SDK dispatch, and 8 CPUs alternating 400 find calls with
400 batch calls using the same per-CPU scratch. No large visited-node set or
runtime allocation is used. Source list mutation/object lifetime is not locked.

These checks do not exercise real Hyper-V scheduling, OS page lifetime changes,
or hardware latency. Historical build/isolated-tests/{Intel,AMD} snapshots are
not the implementation compiled by this suite.
