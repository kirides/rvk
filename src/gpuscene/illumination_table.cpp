#include "gpuscene/illumination_table.h"
#include "gpuscene/verify.h"
#include "gpuscene/draw_record_table.h"
#include "gpuscene/cluster_table.h"
#include "gpuscene/bone_table.h"
#include "enh/rt_gpu_build.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <string>

#include <spdlog/spdlog.h>

#include "d3d9/device.h"
#include "engine/memread.h"
#include "gpuscene/mesh_layout.h"
#include "gpuscene/material_compose.h"
#include "gpuscene/window_compose.h"
#include "shader/abi.h"
#include "gpuscene/object_compose.h"
#include "gpuscene/lighting_images.h"

namespace rvk::gpuscene {

namespace {
constexpr std::uint64_t kVerifyEvery = 240;
constexpr std::uint32_t kNone = 0xffffffffu;
}  // namespace

// ---- GpuTable -------------------------------------------------------------------------------------------------------------------

template <class T>
GpuTable<T>::~GpuTable() = default;

template <class T>
bool GpuTable<T>::create(d3d9::Device& dev, std::uint32_t capacity, const char* name, std::uint64_t padBytes) {
    rhi::Device* rd = dev.rhi();
    if (rd == nullptr) return false;
    pad_ = padBytes;
    buffer_ = rd->createBuffer({static_cast<std::uint64_t>(capacity) * sizeof(T) + pad_,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rhi::Memory::Device, name});
    if (!buffer_) return false;
    const auto w = buffer_->shaderRef(pad_);
    ref_ = rhi::GpuRef::fromWords(w[0], w[1]);
    init(capacity);
    spdlog::info("native: gpu table: {} buffer {} slots x {} B = {} KiB (ref {:#x})", name, capacity, sizeof(T), capacity * sizeof(T) / 1024, ref_.bits);
    return true;
}

template <class T>
void GpuTable<T>::init(std::uint32_t capacity) {
    capacity_ = capacity;
    if (shadow_.size() < capacity) shadow_.resize(capacity);
    if (isDirty_.size() < capacity) isDirty_.resize(capacity, 0);
}

template <class T>
void GpuTable<T>::mark(std::uint32_t i) {
    if (i >= isDirty_.size()) {
        ++stats_.overflow;
        return;
    }
    if (isDirty_[i] == 0) {
        isDirty_[i] = 1;
        dirty_.push_back(i);
    }
}

template <class T>
void GpuTable<T>::markAll(std::uint32_t used) {
    // (every slot that has ever been used, live or freed: the first upload after the buffer's creation)
    for (std::uint32_t i = 0; i < used && i < shadow_.size(); ++i) mark(i);
}

template <class T>
void GpuTable<T>::flush(d3d9::Device& dev, bool verify, std::uint32_t used) {
    if (!buffer_) return;
    rhi::GameDevice& game = dev.game();
    rhi::CommandList* list = nullptr;
    if (!dirty_.empty()) {
        std::sort(dirty_.begin(), dirty_.end());
        const std::uint64_t bytes = static_cast<std::uint64_t>(dirty_.size()) * sizeof(T);
        rhi::RingAlloc a = game.allocRing(bytes, 16);
        if (a.ptr == nullptr) return;  // (kept dirty: the next present)
        auto* dst = static_cast<std::uint8_t*>(a.ptr);
        for (const std::uint32_t i : dirty_) {
            std::memcpy(dst, &shadow_[i], sizeof(T));
            dst += sizeof(T);
        }
        list = &dev.uploadList();
        rhi::Buffer& ring = game.ringBuffer(a);
        std::uint64_t pos = 0;
        for (std::size_t k = 0; k < dirty_.size();) {
            std::size_t e = k + 1;
            while (e < dirty_.size() && dirty_[e] == dirty_[e - 1] + 1) ++e;
            const std::uint64_t n = (e - k) * sizeof(T);
            list->copyBuffer(*buffer_, pad_ + static_cast<std::uint64_t>(dirty_[k]) * sizeof(T), ring, a.offset + pos, n);
            pos += n;
            ++stats_.runs;
            k = e;
        }
        stats_.slotsUploaded += dirty_.size();
        stats_.bytes += bytes;
        ++stats_.flushes;
        for (const std::uint32_t i : dirty_) isDirty_[i] = 0;
        dirty_.clear();
    }
    if (verify) {
        startVerify(dev, dev.uploadList(), used);
    } else if (list != nullptr) {
        list->bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    }
}

template <class T>
bool GpuTable<T>::flushTo(d3d9::Device& dev, rhi::CommandList& list, bool verify, std::uint32_t used) {
    if (!buffer_) return false;
    rhi::GameDevice& game = dev.game();
    bool wrote = false;
    if (!dirty_.empty()) {
        std::sort(dirty_.begin(), dirty_.end());
        const std::uint64_t bytes = static_cast<std::uint64_t>(dirty_.size()) * sizeof(T);
        rhi::RingAlloc a = game.allocRing(bytes, 16);
        if (a.ptr == nullptr) return false;  // (kept dirty)
        auto* dst = static_cast<std::uint8_t*>(a.ptr);
        for (const std::uint32_t i : dirty_) {
            std::memcpy(dst, &shadow_[i], sizeof(T));
            dst += sizeof(T);
        }
        rhi::Buffer& ring = game.ringBuffer(a);
        std::uint64_t pos = 0;
        for (std::size_t k = 0; k < dirty_.size();) {
            std::size_t e = k + 1;
            while (e < dirty_.size() && dirty_[e] == dirty_[e - 1] + 1) ++e;
            const std::uint64_t n = (e - k) * sizeof(T);
            list.copyBuffer(*buffer_, pad_ + static_cast<std::uint64_t>(dirty_[k]) * sizeof(T), ring, a.offset + pos, n);
            pos += n;
            ++stats_.runs;
            k = e;
        }
        stats_.slotsUploaded += dirty_.size();
        stats_.bytes += bytes;
        ++stats_.flushes;
        for (const std::uint32_t i : dirty_) isDirty_[i] = 0;
        dirty_.clear();
        wrote = true;
    }
    if (verify) {
        startVerify(dev, list, used);
    } else if (wrote) {
        list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    }
    return true;
}

template <class T>
void GpuTable<T>::zeroTo(rhi::CommandList& list) {
    if (!buffer_) return;
    list.fillBuffer(*buffer_, 0, buffer_->desc().size, 0);
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
}

template <class T>
void GpuTable<T>::startVerify(d3d9::Device& dev, rhi::CommandList& list, std::uint32_t used) {
    rhi::GameDevice& game = dev.game();
    Pending& p = pending_[game.frameSlot() % rhi::kFramesInFlight];
    if (p.busy) {
        list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
        return;
    }
    if (used == 0) {  // nothing served yet: a read-back would compare never-written GPU memory (the bone table flake)
        list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
        return;
    }
    const std::uint64_t bytes = static_cast<std::uint64_t>(used) * sizeof(T);
    if (!p.buffer || p.buffer->desc().size < bytes) {
        p.buffer = dev.rhi()->createBuffer({static_cast<std::uint64_t>(capacity_) * sizeof(T), VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::Memory::Readback, "gpu table verify"});
        if (!p.buffer) return;
    }
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);  // the uploads' copies are complete before the read-back copies them
    list.copyBuffer(*p.buffer, 0, *buffer_, pad_, bytes);
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
    p.expect.assign(shadow_.begin(), shadow_.begin() + used);
    p.frameValue = game.frameValue();
    p.busy = true;
    ++stats_.verifies;
}

template <class T>
void GpuTable<T>::collect(rhi::GameDevice& game, const char* name) {
    for (Pending& p : pending_) {
        if (!p.busy || game.completedFrame() < p.frameValue) continue;
        p.busy = false;
        p.buffer->invalidate();
        const auto* got = static_cast<const T*>(p.buffer->map());
        std::uint64_t bad = 0;
        for (std::size_t i = 0; i < p.expect.size(); ++i) {
            if (std::memcmp(&got[i], &p.expect[i], sizeof(T)) != 0) {
                if (bad == 0) spdlog::error("native: gpu table: {} slot {} differs between the GPU table and the uploaded copy", name, i);
                ++bad;
            }
        }
        ++stats_.verifyDone;
        stats_.verifySlots += p.expect.size();
        stats_.verifyBad += bad;
        p.expect.clear();
    }
}

template <class T>
void GpuTable<T>::zero(d3d9::Device& dev) {
    if (!buffer_) return;
    rhi::CommandList& list = dev.uploadList();
    list.fillBuffer(*buffer_, 0, buffer_->desc().size, 0);
    list.bufferBarrier(*buffer_, rhi::BufferAccess::ShaderRead);
}

template <class T>
void GpuTable<T>::release() {
    buffer_.reset();
    ref_ = {};
    for (Pending& p : pending_) {
        p.buffer.reset();
        p.expect.clear();
        p.busy = false;
    }
    dirty_.clear();
    std::fill(isDirty_.begin(), isDirty_.end(), 0);
}

template class GpuTable<IllumRecord>;
template class GpuTable<LightSetRecord>;
template class GpuTable<PartRecord>;
template class GpuTable<LayoutRecord>;
template class GpuTable<LightingJob>;  // the lighting images' jobs (lighting_images.cpp)
template class GpuTable<MaterialRecord>;  // the material table (material_table.cpp)
template class GpuTable<ViewRecord>;      // the view blocks (window_tables.cpp)
template class GpuTable<ObjectRecord>;    // the object table (object_table.cpp)
template class GpuTable<shader::abi::ViewBlock2>;  // the view table (view_table.cpp)
template class GpuTable<BoneRecord>;  // the bone table (bone_table.cpp)
template class GpuTable<shader::abi::DrawRecord>;  // the persistent draw records / blocks (draw_record_table.cpp)
template class GpuTable<shader::abi::DrawBlock>;
template class GpuTable<shader::abi::VertexFetch>;  // S-4: the persistent draws' vertex fetch blocks (draw_record_table.cpp)
template class GpuTable<ZeroBlock>;
template class GpuTable<ClusterRow>;  // the cluster table (cluster_table.cpp, F5-1)
template class GpuTable<enh::rtbuild::InstanceInput>;  // the RT scene's GPU instance build (enh/rt_gpu_tables.cpp, frame-ownership.md 14.43)
template class GpuTable<enh::rtbuild::BlasRow>;
template class GpuTable<enh::rtbuild::MaterialRow>;

// ---- IllumTable ------------------------------------------------------------------------------------------------------------------

IllumTable& illumtable() {
    static IllumTable t;
    return t;
}

void IllumTable::releaseGpu() {
    std::lock_guard lock(mutex_);
    illums_.release();
    sets_.release();
}

std::uint32_t IllumTable::allocIllum() {
    if (illumKey_.empty()) {
        illumKey_.assign(kIllumCapacity, nullptr);
        illumSeen_.assign(kIllumCapacity, 0);
        illumFChecked_.assign(kIllumCapacity, 0);
        illums_.init(kIllumCapacity);
        sets_.init(kSetCapacity);
    }
    std::uint32_t slot = kNone;
    if (!freeIllums_.empty()) {
        slot = freeIllums_.back();
        freeIllums_.pop_back();
    } else if (illumUsed_ < kIllumCapacity) {
        slot = illumUsed_++;
    }
    return slot;
}

bool IllumTable::readSetInto(const void* list, LightSetRecord& rec) {
    std::uint8_t raw[kLightSetRawBytes];
    if (!engine::safeRead(list, raw, sizeof(raw))) {
        ++stats_.readFailed;
        return false;
    }
    std::memcpy(rec.raw, raw, sizeof(raw));
    return true;
}

std::uint32_t IllumTable::setSlotFor(const void* list, bool* isNew) {
    if (isNew != nullptr) *isNew = false;
    if (list == nullptr) return kNoSet;
    std::uint32_t slot = kNone;
    for (std::uint32_t i = 0; i < setUsed_; ++i) {
        if (setKey_[i] == list) {
            setSeen_[i] = frame_;
            return i;
        }
        if (setKey_[i] == nullptr && slot == kNone) slot = i;
    }
    if (slot == kNone) {
        if (setUsed_ >= kSetCapacity) {
            ++stats_.setOverflow;
            return kNoSet;
        }
        slot = setUsed_++;
    }
    LightSetRecord& rec = sets_.shadow()[slot];
    std::memset(&rec, 0, sizeof(rec));
    if (!readSetInto(list, rec)) {
        if (slot + 1 == setUsed_) --setUsed_;
        return kNoSet;
    }
    rec.info[0] = 1;
    rec.info[1] = 1;
    rec.info[2] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(list));
    rec.info[3] = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(list) >> 32);
    setKey_[slot] = list;
    setSeen_[slot] = frame_;
    sets_.mark(slot);
    if (isNew != nullptr) *isNew = true;
    return slot;
}

bool IllumTable::refresh(std::uint32_t slot, const void* illum) {
    std::uint8_t raw[kIllumRawBytes];
    if (!engine::safeRead(illum, raw, sizeof(raw))) {
        ++stats_.readFailed;
        return false;
    }
    const void* list = nullptr;
    std::memcpy(&list, raw, sizeof(list));
    IllumRecord n = illums_.shadow()[slot];
    const IllumRecord old = n;
    n.info[0] = 1;
    n.info[1] = setSlotFor(list, nullptr);
    n.info[3] = 0;
    std::memcpy(n.raw, raw, sizeof(raw));
    std::memset(n.raw, 0, 8);
    const bool changed = old.info[0] != n.info[0] || old.info[1] != n.info[1] || std::memcmp(old.raw, n.raw, sizeof(n.raw)) != 0;
    if (changed) {
        ++n.info[2];
        illums_.shadow()[slot] = n;
        illums_.mark(slot);
    }
    return changed;
}

void IllumTable::poll(std::uint32_t slot, const void* illum) {
    // (c) once per frame, for the objects drawn: the object's bytes against the record. A changed f alone = `f`; anything else changed without a
    // hook = `poll` (objects the engine rewrites in place outside eCIlluminated_PS::UpdateIllumination, e.g. per-batch illumination copies).
    // The list pointer (+0) is compared with the key of the record's set (14.30: an object whose list pointer was re-pointed without SetLightSet,
    // e.g. by an illumination copy, kept the old set's record while its draws read the new list: the light spell's set at its spawn)
    if (illumFChecked_[slot] == frame_ + 1) return;
    illumFChecked_[slot] = frame_ + 1;
    const IllumRecord& rec = illums_.shadow()[slot];
    std::uint8_t raw[kIllumRawBytes];
    if (!engine::safeRead(illum, raw, sizeof(raw))) return;
    const void* list = nullptr;
    std::memcpy(&list, raw, sizeof(list));
    const std::uint32_t set = rec.info[1];
    const void* recList = set < setUsed_ ? setKey_[set] : nullptr;
    bool onlyF = list == recList, differs = list != recList;
    for (std::uint32_t o = 8; o < kIllumRawBytes; ++o) {
        if (raw[o] != rec.raw[o]) {
            differs = true;
            if (o >= 12) onlyF = false;
        }
    }
    if (!differs || !refresh(slot, illum)) return;
    if (list != recList) {
        if (stats_.uploadList++ < 4)
            spdlog::info("native: illum table: illum {} slot {}: list pointer {} differs from the record's set (slot {}, list {}) without SetLightSet: re-read", illum, slot, list, set, recList);
    }
    else if (onlyF) ++stats_.uploadF;
    else ++stats_.uploadPoll;
}

std::uint32_t IllumTable::lookupOrCreate(const void* illum) {
    const auto it = illumSlot_.find(illum);
    if (it != illumSlot_.end()) return it->second;
    const std::uint32_t slot = allocIllum();
    if (slot == kNone) {
        ++stats_.illumOverflow;
        return kNone;
    }
    std::memset(&illums_.shadow()[slot], 0, sizeof(IllumRecord));
    illumSlot_[illum] = slot;
    illumKey_[slot] = illum;
    illumSeen_[slot] = frame_;
    illumFChecked_[slot] = 0;
    ++liveIllums_;
    peakIllums_ = std::max(peakIllums_, liveIllums_);
    if (refresh(slot, illum)) ++stats_.uploadNew;
    return slot;
}

void IllumTable::onPsUpdate(const void* ps, bool wasDirty) {
    if (!active_ || !wasDirty || ps == nullptr) return;
    const void* illum = static_cast<const std::uint8_t*>(ps) + 0x78;
    std::lock_guard lock(mutex_);
    const auto it = illumSlot_.find(illum);
    if (it == illumSlot_.end()) return;
    illumSeen_[it->second] = frame_;
    if (refresh(it->second, illum)) ++stats_.uploadDirty;
    else ++stats_.dirtyNoChange;
}

void IllumTable::onSetLightSet(const void* ps) {
    if (!active_ || ps == nullptr) return;
    const void* illum = static_cast<const std::uint8_t*>(ps) + 0x78;
    std::lock_guard lock(mutex_);
    const auto it = illumSlot_.find(illum);
    if (it == illumSlot_.end()) return;
    if (refresh(it->second, illum)) ++stats_.uploadSet;
    else ++stats_.setNoChange;
}

void IllumTable::onSetUpdated(const void* set, bool fromLight) {
    if (!active_ || set == nullptr) return;
    const void* list = static_cast<const std::uint8_t*>(set) + 0x10;
    std::lock_guard lock(mutex_);
    for (std::uint32_t i = 0; i < setUsed_; ++i) {
        if (setKey_[i] != list) continue;
        setSeen_[i] = frame_;
        if (fromLight) ++stats_.lightUpdates;
        LightSetRecord n = sets_.shadow()[i];
        if (!readSetInto(list, n)) return;
        if (std::memcmp(n.raw, sets_.shadow()[i].raw, sizeof(n.raw)) != 0) {
            ++n.info[1];
            sets_.shadow()[i] = n;
            sets_.mark(i);
            ++stats_.uploadSetData;
            if (fromLight) ++stats_.uploadLightData;
        }
        return;
    }
}

void IllumTable::onSetsUpdated() {
    if (!active_) return;
    std::lock_guard lock(mutex_);
    for (std::uint32_t i = 0; i < setUsed_; ++i) {
        if (setKey_[i] == nullptr) continue;
        LightSetRecord n = sets_.shadow()[i];
        if (!readSetInto(setKey_[i], n)) continue;
        if (std::memcmp(n.raw, sets_.shadow()[i].raw, sizeof(n.raw)) != 0) {
            ++n.info[1];
            sets_.shadow()[i] = n;
            sets_.mark(i);
            ++stats_.uploadSetData;
        }
    }
}

void IllumTable::onPsDestroy(const void* ps) {
    if (!active_ || ps == nullptr) return;
    const void* illum = static_cast<const std::uint8_t*>(ps) + 0x78;
    std::lock_guard lock(mutex_);
    const auto it = illumSlot_.find(illum);
    if (it == illumSlot_.end()) return;
    const std::uint32_t slot = it->second;
    illumSlot_.erase(it);
    illumKey_[slot] = nullptr;
    std::memset(&illums_.shadow()[slot], 0, sizeof(IllumRecord));
    illums_.mark(slot);
    freeIllums_.push_back(slot);
    --liveIllums_;
    ++stats_.destroyed;
    ++stats_.freed;
}

std::uint32_t IllumTable::checkDraw(const void* illum, const ComposeParams& p, const float prior[24][4], const float now[24][4], bool verify) {
    if (!active_ || illum == nullptr) return kNoSlot;
    std::lock_guard lock(mutex_);
    const std::uint32_t slot = lookupOrCreate(illum);
    if (slot == kNone) {
        ++stats_.unchecked;
        return kNoSlot;
    }
    illumSeen_[slot] = frame_;
    IllumRecord& rec = illums_.shadow()[slot];
    poll(slot, illum);  // (c): the once-per-frame byte poll
    const std::uint32_t setSlot = rec.info[1];
    if (setSlot >= setUsed_ || setKey_[setSlot] == nullptr) {
        ++stats_.unchecked;
        return slot;
    }
    if (!verify) return slot;
    float exp[24][4];
    std::memcpy(exp, prior, sizeof(exp));
    if (!composeLighting(rec, sets_.shadow()[setSlot], p, exp)) {
        ++stats_.unchecked;
        return slot;
    }
    ++stats_.checked;
    if (std::memcmp(exp, now, sizeof(exp)) == 0) return slot;
    ++stats_.differing;
    if (differingLogged_ < 8) {
        ++differingLogged_;
        std::string regs;
        for (int r = 0; r < 24; ++r) {
            if (std::memcmp(exp[r], now[r], 16) == 0) continue;
            regs += std::format(" c{}: table ({:g} {:g} {:g} {:g}) vs written ({:g} {:g} {:g} {:g});", 14 + r, exp[r][0], exp[r][1], exp[r][2], exp[r][3], now[r][0],
                                now[r][1], now[r][2], now[r][3]);
        }
        spdlog::error("native: illum table: DIFFERING draw: illum {} slot {} set slot {} gen {} (tableLit {}, holdsAll {}, n {}, first {}):{}", illum, slot, setSlot, rec.info[2],
                      p.tableLit, p.holdsAll, p.n, p.firstLight, regs);
    }
    return slot;
}

bool IllumTable::pollDraw(const void* illum, std::uint32_t expectSlot) {
    if (!active_ || illum == nullptr) return false;
    std::lock_guard lock(mutex_);
    const auto it = illumSlot_.find(illum);
    if (it == illumSlot_.end() || it->second != expectSlot) return false;
    const std::uint32_t slot = it->second;
    illumSeen_[slot] = frame_;
    poll(slot, illum);
    return true;
}

std::uint32_t IllumTable::ensureSlot(const void* illum) {
    if (!active_ || illum == nullptr) return kNoSlot;
    std::lock_guard lock(mutex_);
    const std::uint32_t slot = lookupOrCreate(illum);
    if (slot == kNone) {
        ++stats_.unchecked;
        return kNoSlot;
    }
    illumSeen_[slot] = frame_;
    poll(slot, illum);

    return slot;
}

bool IllumTable::compose(std::uint32_t slot, const ComposeParams& p, float L[24][4], int* why) {
    std::lock_guard lock(mutex_);
    if (!active_ || slot >= illumUsed_ || illumKey_.empty() || illumKey_[slot] == nullptr) {
        if (why != nullptr) *why = 1;
        return false;
    }
    const IllumRecord& rec = illums_.shadow()[slot];
    const std::uint32_t setSlot = rec.info[1];
    if (setSlot >= setUsed_ || setKey_[setSlot] == nullptr) {
        if (why != nullptr) *why = 2;
        return false;
    }
    composeLighting(rec, sets_.shadow()[setSlot], p, L);
    return true;
}

void IllumTable::flush(d3d9::Device& dev, bool verify) {
    ++frame_;
    if (!active_) return;
    std::lock_guard lock(mutex_);
    if (illumUsed_ == 0 && setUsed_ == 0) return;
    if (!gpuFailed_ && (!illums_.ready() || !sets_.ready())) {
        if (!sets_.ready() && !sets_.create(dev, kSetCapacity, "illum table light sets")) gpuFailed_ = true;
        else if (!illums_.ready() && !illums_.create(dev, kIllumCapacity, "illum table illuminations")) gpuFailed_ = true;
        if (gpuFailed_) spdlog::warn("native: illum table: the GPU buffers could not be created (the CPU records and the draw check keep running)");
        else {
            sets_.markAll(setUsed_);
            illums_.markAll(illumUsed_);
        }
    }
    // idle sweep: objects not drawn / updated for a long time, light sets not updated
    if (frame_ % 300 == 0) {
        for (std::uint32_t s = 0; s < illumUsed_; ++s) {
            if (illumKey_[s] == nullptr || frame_ - illumSeen_[s] < kIllumIdleFrames) continue;
            illumSlot_.erase(illumKey_[s]);
            illumKey_[s] = nullptr;
            std::memset(&illums_.shadow()[s], 0, sizeof(IllumRecord));
            illums_.mark(s);
            freeIllums_.push_back(s);
            --liveIllums_;
            ++stats_.idleFreed;
            ++stats_.freed;
        }
        // (F-VAR2, frame-ownership.md 7.45: a light set that no hook touched for kSetIdleFrames (a static room's) is still the set of every live illumination that points at it: freeing it
        // left those records with a dangling set, IllumTable::compose answered "no set" and the owned draws' ambient test went false, ~8 ops per view refused with `variant`)
        std::vector<std::uint8_t> referenced(setUsed_, 0);
        for (std::uint32_t s = 0; s < illumUsed_; ++s) {
            if (illumKey_[s] == nullptr) continue;
            const std::uint32_t set = illums_.shadow()[s].info[1];
            if (set < setUsed_) referenced[set] = 1;
        }
        for (std::uint32_t i = 0; i < setUsed_; ++i) {
            if (setKey_[i] == nullptr || frame_ - setSeen_[i] < kSetIdleFrames || referenced[i] != 0) continue;
            setKey_[i] = nullptr;
            std::memset(&sets_.shadow()[i], 0, sizeof(LightSetRecord));
            sets_.mark(i);
            ++stats_.setsFreed;
        }
    }
    if (!gpuFailed_) {
        const bool verifyNow = verify && periodic(frame_, kVerifyEvery);
        sets_.flush(dev, verifyNow, setUsed_);
        illums_.flush(dev, verifyNow, illumUsed_);
        sets_.collect(dev.game(), "light set");
        illums_.collect(dev.game(), "illumination");
    }
    report();
}

void IllumTable::report() {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - lastReport_ < 5.0) return;
    lastReport_ = now;
    const auto& is = illums_.stats();
    const auto& ss = sets_.stats();
    const std::uint64_t bytes = is.bytes + ss.bytes, slots = is.slotsUploaded + ss.slotsUploaded;
    const std::uint64_t frames = std::max<std::uint64_t>(frame_ - lastFrame_, 1);
    std::uint32_t liveSets = 0;
    for (std::uint32_t i = 0; i < setUsed_; ++i) liveSets += setKey_[i] != nullptr ? 1u : 0u;
    spdlog::info(
        "native: illum table (5 s): {} records (peak {}, slots {}), {} sets; uploads: new {}, dirty {}, set {}, f {}, poll {}, list {} (owned ops refused for it {}), set data {} (moving lights: {} re-reads, {} changed); hook without change: dirty {}, set {}; freed {} "
        "(destroyed {}, idle {}, sets {}); {:.1f} records / {:.2f} KiB per frame (GPU buffers: {} + {} records flushed); draws checked {} (unchecked {}) DIFFERING {}; "
        "verify: illums {} read-backs ({} slots) DIFFERING {}, sets {} read-backs DIFFERING {}; overflow illum {} set {} gpu {}, read failures {}",
        liveIllums_, peakIllums_, illumUsed_, liveSets, stats_.uploadNew, stats_.uploadDirty, stats_.uploadSet, stats_.uploadF, stats_.uploadPoll, stats_.uploadList, stats_.liveListMoved, stats_.uploadSetData, stats_.lightUpdates, stats_.uploadLightData, stats_.dirtyNoChange,
        stats_.setNoChange, stats_.freed, stats_.destroyed, stats_.idleFreed, stats_.setsFreed, static_cast<double>(slots - lastSlots_) / static_cast<double>(frames),
        static_cast<double>(bytes - lastBytes_) / 1024.0 / static_cast<double>(frames), is.slotsUploaded, ss.slotsUploaded, stats_.checked, stats_.unchecked, stats_.differing,
        is.verifyDone, is.verifySlots, is.verifyBad, ss.verifyDone, ss.verifyBad, stats_.illumOverflow, stats_.setOverflow, is.overflow + ss.overflow, stats_.readFailed);
    lastFrame_ = frame_;
    lastBytes_ = bytes;
    lastSlots_ = slots;
}

}  // namespace rvk::gpuscene
