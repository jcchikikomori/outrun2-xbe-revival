// Hooks for OutRun 2's default.xbe (title 53450036, XDK 5849, 64 partitions).
// A strong definition replaces the recompiled function at that address, for example:
//
// #include "xdk/hle.h"
//
// X86_EXTERN_FUNC(__imp__sub_00012345);
// X86_FUNC(sub_00012345) {
//     __imp__sub_00012345(ctx, base);  // the original
// }
//
// Instruction-level hooks are declared in config/outrun2.toml ([[hook]]) and implemented here.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_set>

#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace {

// sub_000CBB70 publishes its semaphore at [resource+0x20] before waiting on it. The worker in
// sub_000CB530 can drain [resource+0x1C] after the requester's first read but before that publish,
// so no release is ever queued. These are the only guest fields the boot hook touches.
constexpr uint32_t kBootPending = 0x1C;
constexpr uint32_t kBootSemaphore = 0x20;

// Guards one resource against two host threads completing the same published semaphore. The hook is
// at the single instruction that publishes the handle, so duplicate calls with the same handle are
// already rejected below; this additionally rejects a nested call on the same resource.
std::mutex g_boot_resource_lock;
std::unordered_set<uint32_t> g_boot_resources;

class BootResourceGuard {
public:
    explicit BootResourceGuard(uint32_t resource) : resource_(resource) {
        std::lock_guard<std::mutex> lock(g_boot_resource_lock);
        entered_ = g_boot_resources.insert(resource_).second;
    }
    ~BootResourceGuard() {
        if (!entered_) return;
        std::lock_guard<std::mutex> lock(g_boot_resource_lock);
        g_boot_resources.erase(resource_);
    }
    bool entered() const { return entered_; }

private:
    uint32_t resource_;
    bool entered_ = false;
};

// The static CDevice (DAT_001a4148 points at it) and the fields D3DDevice_SetVertexShader keeps.
// Its guest body (0x00196DF0) stores the object it bound at +0x794 and the handle it was bound as
// at +0x798; the SDK's HLE for that function replaces the body, so the image itself never runs it.
constexpr uint32_t kDevice = 0x001A4150;
constexpr uint32_t kDeviceShaderObject = kDevice + 0x794;  // D3DVertexShader the draws read
constexpr uint32_t kDeviceShaderHandle = kDevice + 0x798;  // the FVF code or shader handle it is bound as
constexpr uint32_t kFvfObject = 0x001A67C8;                // DAT_001a67c8, the FVF declaration object
constexpr uint32_t kSlotOrder = 0x002D6A44;        // slot to declaration-slot bytes (16 + 16)
constexpr uint32_t kComponents = 0x001A1E14;       // components of a format's high nibble
constexpr uint32_t kComponentBytes = 0x001A1E0C;   // bytes of a format's low nibble
constexpr uint32_t kTexDims = 0x001A20C0;          // floats of a texcoord dimension (2 bits a stage)
constexpr uint32_t kDeviceLayout = kDevice + 0xBC8;  // 0x001A4D18: total dwords, first offset, first - end, pairs
constexpr uint32_t kDeviceInputs = kDevice + 0xC54;  // 0x001A4DA4: cached input count

// FUN_00196200's declaration records for an FVF code, built locally.
//
// The guest's SetVertexShader (0x00196DF0) would build them into the object at 0x001A67C8, but that
// setter is HLE'd away and the SDK keeps its own widened declaration in the same object (it points
// the device's vertex-shader field at it in Direct3D_CreateDevice), so guest memory is left alone:
// the records are built into a local array instead, in the same 16 slots the guest builder uses.
// Each record is (byte offset, format); format 2 is D3DDECLTYPE_UNUSED. A format encodes
// (dwords << 4) | kind, which FUN_0019E9E0's tables (0x001A1E0C/0x001A1E14) decode.
// Derived from the Ghidra decompile of 0x00196200 (2026-11-19).
void BuildFvfDeclaration(uint32_t fvf, uint32_t recs[16][2]) {
    for (uint32_t k = 0; k < 16; k++) {
        recs[k][0] = 0;
        recs[k][1] = 2;
    }
    uint32_t u = 0;  // the next record's byte offset
    const uint32_t pos = fvf & 0xE;
    if (pos == 4) {  // XYZRHW
        recs[0][1] = 0x42;
        u = 0x10;
    } else if (pos != 0) {  // XYZ, with optional blends
        recs[0][1] = 0x32;
        u = 0xC;
        if (pos > 5) {  // XYZBn: n = ((pos - 6) >> 1) + 1
            const uint32_t n = ((pos - 6) >> 1) + 1;
            recs[1][0] = 0xC;
            recs[1][1] = n * 0x10 | 2;
            u = n * 4 + 0xC;
        }
    }
    if (fvf & 0x10) {  // NORMAL
        recs[2][0] = u;
        recs[2][1] = 0x32;
        u += 0xC;
    }
    if (fvf & 0x40) {  // DIFFUSE
        recs[3][0] = u;
        recs[3][1] = 0x40;
        u += 4;
    }
    if ((int8_t)fvf < 0) {  // SPECULAR
        recs[4][0] = u;
        recs[4][1] = 0x40;
        u += 4;
    }
    const uint32_t stages = (fvf >> 8) & 0xF;  // texture stages
    uint32_t dims = fvf >> 0x10;               // 2 bits a stage: floats, via 0x001A20C0
    for (uint32_t s = 0; s < stages; s++) {
        const uint32_t floats = kernel::R32(kTexDims + (dims & 3) * 4);
        recs[9 + s][0] = u;
        recs[9 + s][1] = floats << 4 | 2;
        u += floats * 4;
        dims >>= 2;
    }
}

// FUN_0019E9E0's layout of the bound declaration, computed here exactly as it does at 0x0019E9E0:
// for each of the 16 slots in the table's order, skip format 2 (unused), turn the format into dwords
// with the two tables, merge an input that starts where the last one ended, otherwise record the gap.
// The result goes into the CDevice cache: total in dwords at +0xBC8, the first input's byte offset at
// +0xBCC, first minus the last input's end at +0xBD0, (size, gap) dword pairs from +0xBD4 and the
// input count at +0xC54 - what the draw's vertex copy loop reads.
void CacheFvfLayout(const uint32_t recs[16][2]) {
    const uint8_t* order = static_cast<const uint8_t*>(kernel::HostPtr(kSlotOrder + (kernel::R32(kFvfObject + 4) & 0x10)));
    const uint8_t* components = static_cast<const uint8_t*>(kernel::HostPtr(kComponents));
    const uint8_t* component_bytes = static_cast<const uint8_t*>(kernel::HostPtr(kComponentBytes));
    uint32_t size[16] = {}, gap[16] = {};
    uint32_t total = 0, end = 0, first = 0, count = 0;
    for (uint32_t slot = 0; slot < 16; slot++) {
        const uint32_t format = recs[uint32_t(order[slot])][1];
        if (format == 2) continue;  // D3DDECLTYPE_UNUSED
        const uint32_t offset = recs[uint32_t(order[slot])][0];
        const uint32_t dwords = (uint32_t(components[(format >> 4) & 0xF]) * uint32_t(component_bytes[format & 0xF]) + 3) >> 2;
        if (count == 0) {
            size[0] = dwords;
            first = offset;
            count = 1;
        } else if (offset == end) {
            size[count - 1] += dwords;
        } else {
            gap[count] = offset - end;
            size[count] = dwords;
            count++;
        }
        total += dwords;
        end = offset + dwords * 4;
    }
    kernel::W32(kDeviceLayout + 0, total);
    kernel::W32(kDeviceLayout + 4, first);
    kernel::W32(kDeviceLayout + 8, first - end);
    for (uint32_t k = 0; k < count; k++) {
        kernel::W32(kDeviceLayout + 12 + k * 8, size[k]);
        if (k) kernel::W32(kDeviceLayout + 8 + k * 8, gap[k]);
    }
    kernel::W32(kDeviceInputs, count);
    char rec_dump[256];
    int n = std::snprintf(rec_dump, sizeof rec_dump, "layout %u dwords, %u inputs, first %u, end %u from", total, count,
                          first, end);
    for (uint32_t k = 0; k < 16 && n > 0 && size_t(n) < sizeof rec_dump; k++)
        if (recs[k][1] != 2) n += std::snprintf(rec_dump + n, sizeof rec_dump - size_t(n), " [%u]=%u/%X", k, recs[k][0],
                                               recs[k][1]);
    LOGR_FIRST(4, "hooks: 00195CF0 %s", rec_dump);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00195CF0);
X86_EXTERN_FUNC(sub_0013CE40);

// D3DDevice_DrawIndexedVerticesUP (0x00195CF0, an XDK routine the SDK does not name or HLE) divides 0x7FF
// by the CDevice's cached vertex layout at its first act: `MOV ECX,ESI; CALL 0x0019E9E0` at 0x00195D04,
// then `DIV dword ptr [ESI+0xBC8]` at 0x00195D6B. FUN_0019E9E0 (0x0019E9E0) is the only writer of that
// cache, but it only rebuilds it when dirty bit 0x20 is set.
//
// Bit 0x20 is set by D3DDevice_SetVertexShader (0x00196DF0, `|0x70` after building the FVF declaration
// with FUN_00196200) and by D3DDevice_SetStreamSource (0x001969D0, `|0x70`). The SDK's HLE bodies for
// both replace the guest ones, so the cache stayed 0 and the OUTRUN MODE banner - FUN_0002AB40's
// `FUN_00195CF0(6, 0x46, indices, base, 0x28)` at 0x0002AE36, the title's only call to this routine -
// divided 0x7FF by zero.
//
// For an FVF code, do what the guest's own SetVertexShader and the draw's FUN_0019E9E0 would have done:
// build the declaration locally with FUN_00196200's writes and cache it with FUN_0019E9E0's arithmetic.
// The layout is then by construction the one the library would have cached - for FVF 0x1D2, 10 dwords
// (0x28 bytes), exactly the draw's stride. The SDK's own widened declaration in the object at 0x001A67C8
// is left untouched: it is what the SDK's HLE reads the FVF slots from, and a vertex shader program
// (handle bit 0 set) needs none of this.
X86_FUNC(sub_00195CF0) {
    const uint32_t handle = d3d::State().vs_handle;  // what the HLE's D3DDevice_SetVertexShader bound
    if (handle && (handle & 1) == 0 && kernel::R32(kDeviceShaderHandle) != handle) {
        uint32_t recs[16][2];
        BuildFvfDeclaration(handle, recs);
        CacheFvfLayout(recs);
        kernel::W32(kDeviceShaderObject, kFvfObject);
        kernel::W32(kDeviceShaderHandle, handle);
        LOGR_FIRST(4, "hooks: 00195CF0 cached the FVF %X layout for the draw's vertex copy", handle);
    }
    __imp__sub_00195CF0(ctx, base);
}

// Instruction hook declared in config/outrun2.toml after 0x000CBBAD (`mov [esi+0x20], eax`), the
// point where sub_000CBB70 has just published the semaphore it is about to wait on. The guest's own
// first pending read is at 0x000CBB96, before the publish; under host-scheduled guest threads the
// worker can take pending from 1 to 0 in that gap and, because [resource+0x20] was still zero, skip
// its release at 0x000CB58A. The requester then waits forever on a count-zero semaphore.
//
// Re-read pending after the publish. If work is still pending, return false and let the original
// wait at 0x000CBBB0 run. If pending is zero, return true so the generated on_true jump lands at
// 0x000CBBB5: the guest itself reloads the published handle, calls its F0795 close wrapper, clears
// [resource+0x20], then runs the callback and free at 0x000CBBC5 onward. No cleanup is duplicated
// here and the wait's successor owns the exact original close/clear sequence.
//
// The handle check rejects a second completion of the same publication; the resource guard rejects
// re-entry from another guest thread while this one is deciding. A worker that had already read the
// handle may still call NtReleaseSemaphore after the close; handles are never reused by the SDK and
// that worker ignores the return value, so it cannot wake or corrupt another object.
bool FixBootSemaphoreRace(X86Context& ctx, uint8_t* base) {
    const uint32_t resource = ctx.esi;
    if (!resource) return false;

    const uint32_t published = X86_MEM32(resource + kBootSemaphore);
    const uint32_t pending = X86_MEM32(resource + kBootPending);
    if (!published || published != ctx.eax) {
        LOGR("hooks: 000CBB70 resource %08X handle %08X slot %08X, publication already completed",
             resource, ctx.eax, published);
        return false;
    }
    if (pending != 0) {
        LOGR("hooks: 000CBB70 resource %08X handle %08X pending %u, keeping the guest wait",
             resource, published, pending);
        return false;
    }

    BootResourceGuard guard(resource);
    if (!guard.entered()) {
        LOGR("hooks: 000CBB70 resource %08X handle %08X, re-entrant completion ignored", resource, published);
        return false;
    }

    // Re-read after taking the guard in case the other thread completed while we were entering it.
    if (X86_MEM32(resource + kBootPending) != 0 || X86_MEM32(resource + kBootSemaphore) != published) {
        LOGR("hooks: 000CBB70 resource %08X handle %08X, completion superseded", resource, published);
        return false;
    }

    LOGR("hooks: 000CBB70 resource %08X handle %08X pending 0, skipped the wait; guest tail closes it",
         resource, published);
    return true;
}

// Class B WX I/O completion starvation (title-specific, not an SDK/kernel semantic change).
//
// Guest WX reads queued by sub_0013C390 are deferred: byte[obj+1]=2 (busy), dword[obj+0x54]=1
// (submit requested), dword[obj+0x58]=0. They only advance through sub_0013CE40 -> sub_0013CD90,
// which submits the ReadFile wrapper (000F221A/sub_0013C960), polls GetOverlappedResult
// (000F2BE3/sub_0013C070), and on completion sets byte[obj+1]=1. The only static pump
// sub_0013D050 is an init-time drain reached through 0004B120; it is not persistent.
//
// sub_00137AD0 is a blocking upper-engine wait: it calls sub_001374A0 once, then loops calling
// sub_00137940 until byte[esi+1]==1 and [esi+0x20]==0. It never pumps WX, so a queued WX read can
// remain pending forever while the upper engine/worker callback (sub_000CB3F0) waits for completion.
// Diag10 showed worker 2C stuck on resource 0107AF50/item 0107B8C0 with pending 1 after
// data1.cvm/Voice1.xwb opens.
//
// These before-hooks run at 00137AE0 (sub_00137AD0 wait loop, before sub_00137940 polls) and at
// 000CB3F0 (worker callback status poll, before reading handle+1). Each invokes the guest's
// existing WX pump sub_0013CE40 and restores all CPU-visible X86Context state (GPRs, flags,
// x87/SSE/MMX/runtime-owned fields as copied) so the original wait/poll observes the same register
// state; only intentional guest-memory side effects from the pump remain.
void PumpWxWhileIoWait(X86Context& ctx, uint8_t* base) {
    const X86Context saved = ctx;
    sub_0013CE40(ctx, base);
    ctx = saved;
}
