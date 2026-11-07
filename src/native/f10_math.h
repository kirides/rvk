#pragma once

// F10-0 (docs/research/frame-ownership.md 7.31): the engine's main-view collection written down as pure math (no engine calls; the data it reads is
// passed in). Sources, all decompiled from Engine.dll this stage:
//   eCFrustumFilter::CullCamera(box, sphere, range) 0x67e8e0, CullCamera(sphere) 0x67eb70, CullCamera(box) 0x67edf0, CullCameraSphere 0x67e690,
//   CullCameraDistance 0x67e580, eCBeamTree::Cull(box) 0x67ce90 / Cull(sphere) 0x67ba30, eCFrustumElementItem::CullNode 0x6710e0,
//   CalculateFadeOut 0x671220, eCRenderContextMesh::UpdateLoD 0x5c74b0, the hierarchy node walk of RequestRenderOps 0x5c75e0.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rvk::native::f10 {

// the beam tree (eCBeamTree, inside eCFrustumFilter at +0xf0): a binary tree of plane indices over a plane array. Layout read from the engine's memory:
// node: int plane @0, BeamNode* @8 (taken when the box / sphere is not entirely in front), BeamNode* @0x10 (the next plane); plane (0x14 bytes): a b c d, uint selector @0x10;
// per selector k: 3 box-float indices at +0x12e8 + 12 k (the corner that is most inside) and +0x1348 + 12 k (the corner that is most outside) of the tree object
struct BeamNode {
    std::int32_t plane;
    std::int32_t pad;
    const BeamNode* inside;   // +8
    const BeamNode* next;     // +0x10
};
static_assert(sizeof(BeamNode) == 0x18);
struct BeamPlane {
    float a, b, c, d;
    std::uint32_t selector;
};
static_assert(sizeof(BeamPlane) == 0x14);

struct Beam {
    const BeamNode* root = nullptr;
    const BeamPlane* planes = nullptr;
    const std::uint32_t* cornerIn = nullptr;   // 3 x selectors: eCBeamTree +0x12e8
    const std::uint32_t* cornerOut = nullptr;  // 3 x selectors: eCBeamTree +0x1348
};

// the frustum filter's per-frame inputs (eCFrustumFilter): the bounding sphere of the frustum (+0x44 radius, +0x48 centre), its AABB (+0x54 min, +0x60 max), the camera position
struct Filter {
    Beam beam;
    float sphereRadius = 0.0f;
    float sphereCenter[3]{};
    float aabbMin[3]{};
    float aabbMax[3]{};
    float camera[3]{};
};

enum Status : int { kCulled = 0, kInside = 1, kPartial = 2 };

inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// eCBeamTree::Cull(bCBox): box = {min xyz, max xyz}. 0 = outside, 1 = inside, 2 = crossing (a literal port of the engine's walk)
inline int beamCullBox(const Beam& t, const float* box) {
    if (t.root == nullptr) return kCulled;
    const BeamNode* stack[50];
    stack[0] = t.root;
    bool crossing = false;
    int sp = 1;
    do {
        int top = sp - 1;
        const BeamNode* node = stack[top];
        const BeamPlane& p = t.planes[node->plane];
        const std::uint32_t* in = t.cornerIn + 3 * p.selector;
        const std::uint32_t* out = t.cornerOut + 3 * p.selector;
        const bool outFront = 0.0f < p.a * box[out[0]] + p.b * box[out[1]] + p.c * box[out[2]] + p.d;  // (the engine's bVar7)
        const bool inFront = 0.0f <= p.a * box[in[0]] + p.b * box[in[1]] + p.c * box[in[2]] + p.d;     // the other corner is in front too
        int next = top;                                                                              // the slot count after this node
        bool takeNext = false;
        if (inFront) {
            takeNext = outFront;
        } else {
            crossing = true;
            if (node->inside != nullptr) {
                stack[top] = node->inside;
                next = sp;
                takeNext = outFront;
            } else if (outFront) {
                takeNext = true;
            } else if (top == 0) {
                return kCulled;
            }
        }
        if (takeNext) {
            if (node->next == nullptr) {
                if (crossing) return kPartial;
            } else {
                stack[next] = node->next;
                ++next;
            }
        }
        sp = next;
    } while (sp != 0);
    return crossing ? kPartial : kInside;
}

// eCBeamTree::Cull(bCSphere): sphere = {radius, centre xyz}
inline int beamCullSphere(const Beam& t, float radius, const float* center) {
    if (t.root == nullptr) return kCulled;
    const BeamNode* stack[50];
    stack[0] = t.root;
    bool crossing = false;
    int sp = 1;
    do {
        int top = sp - 1;
        const BeamNode* node = stack[top];
        const BeamPlane& p = t.planes[node->plane];
        const float dist = p.a * center[0] + p.b * center[1] + p.c * center[2] + p.d;
        const bool someFront = 0.0f <= radius + dist;
        const bool allFront = 0.0f < dist - radius;
        int next = top;
        bool takeNext = false;
        if (allFront) {
            takeNext = someFront;
        } else {
            crossing = true;
            if (node->inside != nullptr) {
                stack[top] = node->inside;
                next = sp;
                takeNext = someFront;
            } else if (someFront) {
                takeNext = true;
            } else if (top == 0) {
                return kCulled;
            }
        }
        if (takeNext) {
            if (node->next == nullptr) {
                if (crossing) return kPartial;
            } else {
                stack[next] = node->next;
                ++next;
            }
        }
        sp = next;
    } while (sp != 0);
    return crossing ? kPartial : kInside;
}

// CullCameraSphere(sphere): the entity's sphere against the frustum's bounding sphere
inline bool cullCameraSphere(const Filter& f, float radius, const float* center) {
    const float d[3] = {center[0] - f.sphereCenter[0], center[1] - f.sphereCenter[1], center[2] - f.sphereCenter[2]};
    const float r = f.sphereRadius + radius;
    return dot3(d, d) <= r * r;
}

// CullCameraDistance(sphere, range): 0 = beyond the range, 1 = inside the range sphere, 2 = straddling its edge
inline int cullCameraDistance(const Filter& f, float radius, const float* center, float range) {
    const float d[3] = {center[0] - f.camera[0], center[1] - f.camera[1], center[2] - f.camera[2]};
    const float d2 = dot3(d, d);
    if ((radius + range) * (radius + range) < d2) return kCulled;
    if (range < radius || (radius - range) * (radius - range) < d2) return kPartial;
    return kInside;
}

// CullCamera(sphere) = the leaf test of OnQueryNodes
inline int cullCameraLeaf(const Filter& f, float radius, const float* center) {
    if (!cullCameraSphere(f, radius, center)) return kCulled;
    return beamCullSphere(f.beam, radius, center);
}

// CullCamera(box, sphere, range) = eCFrustumElementItem::CullNode's test (range already multiplied by the view range factor; 0 = no distance cull)
inline int cullCameraFull(const Filter& f, const float* box, float radius, const float* center, float range) {
    if (!cullCameraSphere(f, radius, center)) return kCulled;
    int dist = kInside;
    if (range > 0.0f) {
        dist = cullCameraDistance(f, radius, center, range);
        if (dist == kCulled) return kCulled;
    }
    int b = beamCullBox(f.beam, box);
    if (b == kCulled) return kCulled;
    if (dist == kPartial && b == kInside) b = kPartial;
    return b;
}

// CullCamera(box) (the hierarchy nodes): the AABB around the frustum's bounding sphere first, then the beam tree
inline int cullCameraBox(const Filter& f, const float* box) {
    if (f.aabbMax[0] < box[0] || box[3] < f.aabbMin[0] || f.aabbMax[1] < box[1] || box[4] < f.aabbMin[1] || f.aabbMax[2] < box[2] || box[5] < f.aabbMin[2]) return kCulled;
    return beamCullBox(f.beam, box);
}

// ExtractFromEntity's gate on the entity flags (+0x28): 0x8026 all set, 8 clear
inline bool entityExtractable(std::uint32_t flags) { return (flags & 0x8026u) == 0x8026u && (flags & 8u) == 0; }

// eCFrustumElementItem::CalculateFadeOut: baseFade = entity +0xc8, viewRange = entity +0xcc, factor = the database's +0x15b4, noRange = db byte +0x15a9,
// camera / centre as in the engine; near = the engine's fraction of the range (DAT_1809f1674) and one = DAT_1809f1698
inline float calculateFade(float baseFade, float viewRange, float factor, bool noRange, const float* camera, const float* center, float epsilon, float nearFraction, float one) {
    if (noRange) return baseFade;
    const float range = viewRange * factor;
    if (range <= epsilon) return baseFade;
    const float d[3] = {camera[0] - center[0], camera[1] - center[1], camera[2] - center[2]};
    const float inner = range * nearFraction;
    const float d2 = dot3(d, d);
    if (d2 <= range * range) {
        if (inner * inner < d2) return baseFade * (one - (std::sqrt(d2) - inner) / (range - inner));
        return baseFade;
    }
    return 0.0f;
}

// UpdateLoD: lodCount = PS +0x6c, ranges = PS +0x60 (lodCount - 1 floats, already scaled by lod.meshes), res[i] = PS +0x40 + 8 i, resident(i) = res state == 3;
// center = the entity's sphere centre, camera = the database's camera. Returns the index of the picked resource (-1 = none resident) and the wanted index for the prefetch (-1 = none)
struct LodPick {
    int chosen = -1;
    int wanted = -1;
};
template <class Resident>
inline LodPick pickLod(int lodCount, const float* ranges, const float* camera, const float* center, float posInfinity, Resident&& resident) {
    LodPick out;
    if (lodCount < 2) return out;
    const float d[3] = {center[0] - camera[0], center[1] - camera[1], center[2] - camera[2]};
    const float d2 = dot3(d, d);
    int wanted = -1;  // peVar5 / peVar3 of the engine's loop: the lowest index whose range still holds the distance
    for (int i = lodCount - 1; i >= 0; --i) {
        const float r = i < lodCount - 1 ? ranges[i] : posInfinity;
        const int cand = (r * r < d2) ? wanted : i;
        if (resident(i) && (d2 <= r * r || out.chosen < 0)) out.chosen = i;
        wanted = cand;
    }
    out.wanted = wanted;
    return out;
}

// the node walk of eCRenderContextMesh::RequestRenderOps for one sub-mesh's node table: 0x28-byte nodes in pre-order, +0 the node after the subtree (the end
// sentinel has 0), +8 the box, +0x20 an ordinal (the range start). `level` = ctx +0x34; emit(nodeIndex) for each node that gets ops; cull(box) = cullCameraBox
struct RawNode {
    const RawNode* skip;
    float box[6];
    std::int32_t ordinal;
    std::int32_t pad;
};
static_assert(sizeof(RawNode) == 0x28);
template <class Cull, class Emit>
inline void walkNodes(const RawNode* base, int level, Cull&& cull, Emit&& emit) {
    for (const RawNode* p = base; p->skip != nullptr;) {
        const int st = cull(p->box);
        if (st == kCulled) {
            p = p->skip;
            continue;
        }
        const bool leaf = p->skip == p + 1;
        const bool take = leaf || (st == kPartial ? static_cast<std::uint32_t>(p->skip->ordinal - p->ordinal) < static_cast<std::uint32_t>(level * 3) : true);
        if (take) {
            emit(static_cast<std::uint32_t>(p - base));
            p = p->skip;
        } else {
            p = p + 1;
        }
    }
}

// db +0x1590 .. +0x15a8 flag bytes -> the request mask of eCFrustumDatabase::Request 0x66e8c0
inline std::uint16_t requestMask(bool b1591, bool b15a7, bool b15a5, bool b15a1, bool b15a8, bool b1590, bool b15a4) {
    std::uint16_t m = b1591 ? 6 : 2;
    if (b15a7) m |= 0x20;
    if (b15a5) m |= 1;
    if (b15a1) m |= 0x40;
    if (b15a8) m |= 0x80;
    if (b1590) m |= 8;
    if (b15a4) m |= 0x100;
    return m;
}

}  // namespace rvk::native::f10
