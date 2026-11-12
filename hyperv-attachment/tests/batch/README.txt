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

Operation 13 (virt_read_dependent_batch) uses the production dependent handler:
64-node chains, shared pointer parents, independent roots, signed offsets,
pointer-inclusive 64 KiB budget, null/noncanonical pointers, overflow/underflow,
failure propagation, malformed/forward/non-pointer parents, control/output
overlap, split descriptors/pointers/payload/results, private pointer/descriptor
snapshots, publication failure, bounds/roots/reentry guards, SDK marshalling,
and 256 randomized 64-item dependency graphs checked against a host data model.
The 8-CPU mixed test alternates 400 calls to EACH of operations 11, 12 and 13,
sharing the same per-CPU workspace. The assembly model also checks operation 13.

The shared ABI is in shared/dependent.hpp (24-byte item, 576-byte result).
SDK entry: hc::read_virtual_dependent_batch(source_cr3, items, count, result).
Set parent=hc::dependent_direct for an absolute source VA. Otherwise address
is a signed byte offset encoded as uint64_t, and parent names an earlier POINTER
item. Pointer items require size=8 and destination_va=0. Copy items require a
nonzero size and a destination in the calling address space. All reserved bytes
must be zero. Controls/outputs must stay resident and exclusively owned; they
must not overlap or physically alias each other or the source data.
Controls/outputs are restricted to the lower canonical user half. Each guest
page-table level must grant user access for descriptors and user/write access
for outputs, including large-page leaves. These permissions are checked on each
page as it is accessed. Source reads retain kernel/readonly inspection support.
Checked output writes also respect SLAT write protection.
Tests include readonly/supervisor pages, ancestor permissions, 1 GiB/2 MiB
output pages and a destination becoming readonly between copy chunks.
allocator-check.vcxproj compiles the REAL slab/page allocators, rebasing their
boot heap onto aligned private host RAM. It checks 16 workspaces, one page per
CPU, coexistence with small slab buckets, reclamation, exhaustion and recovery.

Example (offsets/data length supplied by the caller, one source CR3):
  items[0] = {root_va + offset_a, 0, 8, hc::dependent_direct,
              hc::dependent_read_kind::pointer, 0};
  items[1] = {offset_b, 0, 8, 0, hc::dependent_read_kind::pointer, 0};
  items[2] = {offset_c, destination_va, data_length, 1,
              hc::dependent_read_kind::copy, 0};
Check bool publication AND result.status[i]==success before using each output.
Successful pointer observations are in result.pointers[i]; other pointer slots
and unused statuses are zero. transfer_failed covers either endpoint and may
leave partial destination data. A false return invalidates ALL outputs, even if
some transfers finished. The SDK clears an incomplete published result; locally
rejected inputs may leave it unchanged (especially overlapping controls).
Source identity/lifetime checks remain the caller's responsibility. No atomic
snapshot, retry loop, cross-call state or automatic target pinning is provided.
