// =============================================================================
//  2 mods  -  "Fling Item Gun" + "Item Spawner Gun"   (Yeeps, blaze build)
//
//  HOW TO USE
//    Hold Y (left hand)     show the menu next to your left hand
//    Right index finger     touch a button to switch that gun on / off
//                           (green = on, red = off, only ONE gun is on at a time)
//
//    With a gun switched on:
//      hold RIGHT GRIP      aim (a small white dot shows where you are aiming)
//      pull RIGHT TRIGGER   fire
//
//    Item Spawner Gun       spawns an item at the dot (shape + colour cycle)
//    Fling Item Gun         flings the item you are aiming at, away from you
//
//  LOCAL ONLY: every item is created on YOUR device and is never sent to other
//  players, the room or the server. No Photon / networking code is used.
//  The Fling gun only works on the items made by the Spawner gun.
//
//  If the gun points the wrong way, change kAimGetter below
//  ("get_forward" -> "get_up" or "get_right").
//  If the labels don't show up, set TWO_MODS_LABELS to 0 (top button = Fling
//  Item Gun, bottom button = Item Spawner Gun).
// =============================================================================
#include "ymsdk.hpp"

#include <cmath>
#include <cstdint>

YEEPS_MOD("2 mods")

using namespace BNM;
using namespace BNM::Structures::Unity;
using BNM::IL2CPP::Il2CppObject;

// ----------------------------------------------------------------------------
//  Settings (safe to tweak)
// ----------------------------------------------------------------------------
#define TWO_MODS_LABELS 1                 // 1 = text on the buttons (TextMesh), 0 = colours only

static const char *kAimGetter     = "get_forward";  // which Transform direction is "pointing"

static const int   kMaxItems      = 24;     // hard cap; the oldest item is recycled
static const float kItemSize      = 0.25f;  // metres
static const float kItemMass      = 0.25f;
static const float kSpawnDist     = 1.2f;   // spawn point, metres in front of your right hand
static const float kSpawnDelay    = 0.18f;  // seconds between spawns while holding the trigger
static const float kFlingSpeed    = 14.0f;  // metres / second
static const float kFlingDelay    = 0.25f;
static const float kFlingRange    = 25.0f;  // how far the fling gun can reach
static const float kAimRadius     = 0.7f;   // how close to the aim line an item must be

static const float kBtnW          = 0.34f;  // menu button size (metres)
static const float kBtnH          = 0.08f;
static const float kMenuLift      = 0.16f;  // menu floats this far above your left hand
static const float kFingerOffset  = 0.07f;  // fingertip = right hand + aim direction * this
static const float kPressDepth    = 0.06f;  // how close to the button face a touch has to be
static const float kPressDelay    = 0.5f;   // seconds between button presses

#if TWO_MODS_LABELS
static const int   kLabelFontSize = 64;
static const float kLabelCharSize = 0.0042f; // text size: raise/lower to taste
#endif

// ----------------------------------------------------------------------------
//  Tiny vector helpers (so we never depend on BNM operators)
// ----------------------------------------------------------------------------
struct V3 { float x, y, z; };
static inline V3 MkV(float x, float y, float z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
static inline V3 operator+(V3 a, V3 b) { return MkV(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 operator-(V3 a, V3 b) { return MkV(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 operator*(V3 a, float s) { return MkV(a.x * s, a.y * s, a.z * s); }
static inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Cross(V3 a, V3 b) {
    return MkV(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
static inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-5f ? a * (1.0f / l) : MkV(0, 0, 1); }
static inline V3 FromU(Vector3 v) { return MkV(v.x, v.y, v.z); }
static inline Vector3 ToU(V3 v) { return Vector3(v.x, v.y, v.z); }

// ----------------------------------------------------------------------------
//  GC handles (keep Unity objects alive between frames)
//  Resolved through the game's own il2cpp, same as everything else in the SDK.
// ----------------------------------------------------------------------------
typedef uint32_t (*GcNewFn)(Il2CppObject *, bool);
typedef Il2CppObject *(*GcGetFn)(uint32_t);
typedef void (*GcFreeFn)(uint32_t);
static GcNewFn  gcNew  = nullptr;
static GcGetFn  gcGet  = nullptr;
static GcFreeFn gcFree = nullptr;

static uint32_t GcHold(Il2CppObject *o) { return (o && gcNew) ? gcNew(o, false) : 0; }
static Il2CppObject *GcGet(uint32_t h) { return (h && gcGet) ? gcGet(h) : nullptr; }
static void GcRelease(uint32_t &h) { if (h && gcFree) gcFree(h); h = 0; }

// ----------------------------------------------------------------------------
//  Lookups: every class / method is found ONCE, on the first frame
// ----------------------------------------------------------------------------
struct XRDevice { uint64_t id; bool initialized; };   // layout of UnityEngine.XR.InputDevice

struct Lookups {
    bool tried = false, ok = false;

    BNM::MonoType *rbType = nullptr, *colType = nullptr, *renType = nullptr;

    // GameObject
    Method<Il2CppObject *> createPrimitive, getTransform, addComponent;
    Method<bool> tryGet;                       // GameObject.TryGetComponent(Type, out Component)
    Method<void> setActive;
    // Transform
    Method<Vector3> getPos, getAim;
    Method<void> setPos, setRot, setScale;
    // UnityEngine.Object
    Method<void> destroy;
    Method<bool> alive;
    // Quaternion
    Method<Quaternion> lookRot;
    // Rigidbody
    Method<void> rbSetVel, rbSetMass;
    // Renderer / Material
    Method<Il2CppObject *> getMaterial;
    Method<void> setColor;
    // Controllers
    Method<XRDevice> getDevice;
    Method<bool> tryBool;
    // MasterPlayer fields
    Field<Il2CppObject *> fLeft, fRight, fHead;

#if TWO_MODS_LABELS
    BNM::MonoType *tmType = nullptr;
    Method<void> tmText, tmCharSize, tmFontSize, tmAnchor, tmAlign, tmColor, tmFont, renSetMat;
    uint32_t font = 0, fontMat = 0;
    bool labelsOk = false;
#endif
};
static Lookups lk;

// ----------------------------------------------------------------------------
//  State
// ----------------------------------------------------------------------------
struct Part { uint32_t go = 0, tr = 0, mat = 0; };
struct Item { bool used = false; uint32_t go = 0, tr = 0, rb = 0; };

struct State {
    Il2CppObject *player = nullptr;
    bool dirty = false;

    uint32_t sSecondary = 0, sGrip = 0, sTrigger = 0;   // C# strings, made once

    int   mode = 0;                       // 0 = off, 1 = fling gun, 2 = spawner gun
    float cool = 0, press = 0;

    bool menuBuilt = false, menuShown = false, menuFailed = false;
    bool dotBuilt = false, dotShown = false, dotFailed = false;
    Part btn[2];                          // 0 = Fling Item Gun, 1 = Item Spawner Gun
    Part label[2];
    Part dot;

    Item items[kMaxItems];
    int nextSlot = 0, shapeIdx = 0, colorIdx = 0;
};
static State st;

static const float kBtnY[2] = {0.05f, -0.05f};          // button heights inside the menu

static Color OnColor()  { return Color(0.15f, 0.85f, 0.30f, 1.0f); }
static Color OffColor() { return Color(0.80f, 0.15f, 0.15f, 1.0f); }
static Color DotColor() { return Color(1.0f, 1.0f, 1.0f, 1.0f); }
static Color PaletteColor(int i) {
    static const float c[6][3] = {
        {0.95f, 0.30f, 0.30f}, {0.95f, 0.65f, 0.20f}, {0.95f, 0.90f, 0.25f},
        {0.30f, 0.85f, 0.40f}, {0.30f, 0.65f, 0.95f}, {0.75f, 0.45f, 0.95f}};
    const float *p = c[i % 6];
    return Color(p[0], p[1], p[2], 1.0f);
}

#define REQUIRE(cond, what)                                   \
    do {                                                      \
        if (!(cond)) {                                        \
            ym::log("[2 mods] missing: %s", what);            \
            return false;                                     \
        }                                                     \
    } while (0)

static uint32_t MakeStr(const char *s) { return GcHold((Il2CppObject *)CreateMonoString(s)); }

static bool EnsureLookups() {
    if (lk.tried) return lk.ok;
    lk.tried = true;

    const YeepsModApi *api = ym::api();
    gcNew  = (GcNewFn)api->resolve("il2cpp_gchandle_new");
    gcGet  = (GcGetFn)api->resolve("il2cpp_gchandle_get_target");
    gcFree = (GcFreeFn)api->resolve("il2cpp_gchandle_free");
    REQUIRE(gcNew && gcGet && gcFree, "il2cpp gchandle functions");

    Class goC("UnityEngine", "GameObject");
    Class trC("UnityEngine", "Transform");
    Class objC("UnityEngine", "Object");
    Class rbC("UnityEngine", "Rigidbody");
    Class matC("UnityEngine", "Material");
    Class renC("UnityEngine", "Renderer");
    Class colC("UnityEngine", "Collider");
    Class quatC("UnityEngine", "Quaternion");
    Class xrC("UnityEngine.XR", "InputDevices");
    Class plC("", "MasterPlayer", Image("Assembly-CSharp.dll"));
    REQUIRE(goC.IsValid() && trC.IsValid() && objC.IsValid() && rbC.IsValid(), "core Unity classes");
    REQUIRE(matC.IsValid() && renC.IsValid() && colC.IsValid() && quatC.IsValid(), "render/physics classes");
    REQUIRE(xrC.IsValid() && plC.IsValid(), "XR InputDevices / MasterPlayer");

    lk.rbType  = rbC.GetMonoType();
    lk.colType = colC.GetMonoType();
    lk.renType = renC.GetMonoType();
    GcHold((Il2CppObject *)lk.rbType);       // keep the System.Type objects alive
    GcHold((Il2CppObject *)lk.colType);
    GcHold((Il2CppObject *)lk.renType);

    lk.createPrimitive = goC.GetMethod("CreatePrimitive", 1);
    lk.getTransform    = goC.GetMethod("get_transform", 0);
    lk.addComponent    = goC.GetMethod("AddComponent", 1);
    lk.tryGet          = goC.GetMethod("TryGetComponent", 2);
    lk.setActive       = goC.GetMethod("SetActive", 1);

    lk.getPos   = trC.GetMethod("get_position", 0);
    lk.getAim   = trC.GetMethod(kAimGetter, 0);
    lk.setPos   = trC.GetMethod("set_position", 1);
    lk.setRot   = trC.GetMethod("set_rotation", 1);
    lk.setScale = trC.GetMethod("set_localScale", 1);

    lk.destroy = objC.GetMethod("Destroy", 1);
    lk.alive   = objC.GetMethod("op_Implicit", 1);
    lk.lookRot = quatC.GetMethod("LookRotation", 2);

    lk.rbSetVel = rbC.GetMethod("set_velocity", 1);
    if (!lk.rbSetVel.IsValid()) lk.rbSetVel = rbC.GetMethod("set_linearVelocity", 1);  // newer Unity
    lk.rbSetMass = rbC.GetMethod("set_mass", 1);

    lk.getMaterial = renC.GetMethod("get_material", 0);
    lk.setColor    = matC.GetMethod("set_color", 1);

    lk.getDevice = xrC.GetMethod("GetDeviceAtXRNode", 1);
    lk.tryBool   = xrC.GetMethod("TryGetFeatureValue_bool", 3);

    lk.fLeft  = plC.GetField("leftHandTransform");
    lk.fRight = plC.GetField("rightHandTransform");
    lk.fHead  = plC.GetField("headTransform");

    REQUIRE(lk.createPrimitive.IsValid() && lk.getTransform.IsValid(), "GameObject.CreatePrimitive/get_transform");
    REQUIRE(lk.addComponent.IsValid() && lk.setActive.IsValid(), "GameObject.AddComponent/SetActive");
    REQUIRE(lk.tryGet.IsValid(), "GameObject.TryGetComponent(Type, out Component)");
    REQUIRE(lk.getPos.IsValid() && lk.getAim.IsValid(), "Transform.get_position / aim getter");
    REQUIRE(lk.setPos.IsValid() && lk.setRot.IsValid() && lk.setScale.IsValid(), "Transform setters");
    REQUIRE(lk.destroy.IsValid() && lk.alive.IsValid(), "Object.Destroy/op_Implicit");
    REQUIRE(lk.lookRot.IsValid(), "Quaternion.LookRotation");
    REQUIRE(lk.rbSetVel.IsValid() && lk.rbSetMass.IsValid(), "Rigidbody velocity/mass");
    REQUIRE(lk.getMaterial.IsValid() && lk.setColor.IsValid(), "Renderer.material / Material.color");
    REQUIRE(lk.getDevice.IsValid() && lk.tryBool.IsValid(), "XR controller input");
    REQUIRE(lk.fLeft.IsValid() && lk.fRight.IsValid() && lk.fHead.IsValid(), "MasterPlayer hand/head fields");

    st.sSecondary = MakeStr("SecondaryButton");   // left Secondary = Y
    st.sGrip      = MakeStr("GripButton");
    st.sTrigger   = MakeStr("TriggerButton");

#if TWO_MODS_LABELS
    // Optional: text labels. Any failure here just turns labels off.
    Class tmC("UnityEngine", "TextMesh");
    Class fontC("UnityEngine", "Font");
    Class resC("UnityEngine", "Resources");
    if (tmC.IsValid() && fontC.IsValid() && resC.IsValid()) {
        lk.tmType   = tmC.GetMonoType();
        GcHold((Il2CppObject *)lk.tmType);
        lk.tmText     = tmC.GetMethod("set_text", 1);
        lk.tmCharSize = tmC.GetMethod("set_characterSize", 1);
        lk.tmFontSize = tmC.GetMethod("set_fontSize", 1);
        lk.tmAnchor   = tmC.GetMethod("set_anchor", 1);
        lk.tmAlign    = tmC.GetMethod("set_alignment", 1);
        lk.tmColor    = tmC.GetMethod("set_color", 1);
        lk.tmFont     = tmC.GetMethod("set_font", 1);
        lk.renSetMat  = renC.GetMethod("set_material", 1);
        Method<Il2CppObject *> getBuiltin = resC.GetMethod("GetBuiltinResource", 2);
        Method<Il2CppObject *> fontGetMat = fontC.GetMethod("get_material", 0);
        if (lk.tmText.IsValid() && lk.tmCharSize.IsValid() && lk.tmFontSize.IsValid() &&
            lk.tmAnchor.IsValid() && lk.tmAlign.IsValid() && lk.tmColor.IsValid() &&
            lk.tmFont.IsValid() && lk.renSetMat.IsValid() && getBuiltin.IsValid() &&
            fontGetMat.IsValid()) {
            BNM::MonoType *fontType = fontC.GetMonoType();
            GcHold((Il2CppObject *)fontType);
            // "LegacyRuntime.ttf" on Unity 2022.2+, "Arial.ttf" before that
            Il2CppObject *f = getBuiltin(fontType, (Il2CppObject *)CreateMonoString("LegacyRuntime.ttf"));
            if (!f) f = getBuiltin(fontType, (Il2CppObject *)CreateMonoString("Arial.ttf"));
            if (f) {
                Il2CppObject *fm = fontGetMat[f]();
                lk.font = GcHold(f);
                lk.fontMat = GcHold(fm);
                lk.labelsOk = lk.font && lk.fontMat;
            }
        }
    }
    if (!lk.labelsOk) ym::log("[2 mods] labels off (TextMesh/font not available) - buttons use colours");
#endif

    lk.ok = true;
    ym::log("[2 mods] ready");
    return true;
}

// ----------------------------------------------------------------------------
//  Small Unity helpers
// ----------------------------------------------------------------------------
static bool IsAlive(Il2CppObject *o) { return o && lk.alive(o); }
static V3 PosOf(Il2CppObject *tr) { return FromU(lk.getPos[tr]()); }
static V3 AimOf(Il2CppObject *tr) { return Norm(FromU(lk.getAim[tr]())); }
static void DestroyObj(Il2CppObject *o) { if (IsAlive(o)) lk.destroy(o); }

static bool ReadBtn(int node, uint32_t strHandle) {       // node: 4 = left hand, 5 = right hand
    Il2CppObject *s = GcGet(strHandle);
    if (!s) return false;
    XRDevice dev = lk.getDevice(node);
    bool v = false;
    lk.tryBool(dev.id, s, &v);
    return v;
}

// Removes the physics collider so menu parts can't shove you around or be climbed on.
static void StripCollider(Il2CppObject *go) {
    Il2CppObject *c = nullptr;
    if (lk.tryGet[go](lk.colType, &c) && c) lk.destroy(c);
}

// Paints a renderer's material; returns the material so it can be recoloured later.
static Il2CppObject *Paint(Il2CppObject *go, Color col) {
    Il2CppObject *ren = nullptr;
    if (!lk.tryGet[go](lk.renType, &ren) || !ren) return nullptr;
    Il2CppObject *m = lk.getMaterial[ren]();
    if (m) lk.setColor[m](col);
    return m;
}

static bool MakePart(Part &p, int primitive, V3 scale, Color col, bool keepCollider) {
    Il2CppObject *go = lk.createPrimitive(primitive);    // PrimitiveType: 0 sphere, 1 capsule, 2 cylinder, 3 cube
    if (!go) return false;
    if (!keepCollider) StripCollider(go);
    Il2CppObject *tr = lk.getTransform[go]();
    if (!tr) { DestroyObj(go); return false; }
    lk.setScale[tr](ToU(scale));
    Il2CppObject *m = Paint(go, col);
    p.go = GcHold(go);
    p.tr = GcHold(tr);
    p.mat = GcHold(m);
    return p.go && p.tr;
}

static void FreePart(Part &p) {
    Il2CppObject *go = GcGet(p.go);
    if (go) DestroyObj(go);
    GcRelease(p.go);
    GcRelease(p.tr);
    GcRelease(p.mat);
}

static void SetPartActive(Part &p, bool on) {
    Il2CppObject *go = GcGet(p.go);
    if (IsAlive(go)) lk.setActive[go](on);
}

static void PlacePart(Part &p, V3 pos, Quaternion rot) {
    Il2CppObject *t = GcGet(p.tr);
    if (!t) return;
    lk.setPos[t](ToU(pos));
    lk.setRot[t](rot);
}

// ----------------------------------------------------------------------------
//  Items (spawner gun makes them, fling gun pushes them)
// ----------------------------------------------------------------------------
static void FreeItem(Item &it) {
    if (!it.used) return;
    Il2CppObject *go = GcGet(it.go);
    if (go) DestroyObj(go);
    GcRelease(it.go);
    GcRelease(it.tr);
    GcRelease(it.rb);
    it.used = false;
}

static void SpawnItem(V3 pos) {
    static const int kShapes[4] = {3, 0, 1, 2};          // cube, sphere, capsule, cylinder

    int slot = st.nextSlot;
    st.nextSlot = (st.nextSlot + 1) % kMaxItems;
    FreeItem(st.items[slot]);                            // recycle the oldest one when full

    Il2CppObject *go = lk.createPrimitive(kShapes[st.shapeIdx]);
    st.shapeIdx = (st.shapeIdx + 1) % 4;
    if (!go) return;
    Il2CppObject *tr = lk.getTransform[go]();
    Il2CppObject *rb = lk.addComponent[go](lk.rbType);   // Rigidbody: gravity + collisions, local only
    if (!tr || !rb) { DestroyObj(go); return; }

    lk.setScale[tr](ToU(MkV(kItemSize, kItemSize, kItemSize)));
    lk.setPos[tr](ToU(pos));
    lk.rbSetMass[rb](kItemMass);
    Paint(go, PaletteColor(st.colorIdx++));

    Item &it = st.items[slot];
    it.go = GcHold(go);
    it.tr = GcHold(tr);
    it.rb = GcHold(rb);
    it.used = it.go && it.tr && it.rb;
}

// Finds the item closest to the aim line. Returns false if none is in range.
static bool AimAtItem(V3 o, V3 d, int &best, V3 &bestPos) {
    best = -1;
    float bestAlong = kFlingRange;
    for (int i = 0; i < kMaxItems; i++) {
        Item &it = st.items[i];
        if (!it.used) continue;
        Il2CppObject *t = GcGet(it.tr);
        if (!IsAlive(t)) { FreeItem(it); continue; }     // gone (scene change etc.)
        V3 p = PosOf(t);
        V3 w = p - o;
        float along = Dot(w, d);
        if (along < 0.15f || along > bestAlong) continue;
        if (Len(w - d * along) > kAimRadius) continue;
        best = i;
        bestAlong = along;
        bestPos = p;
    }
    return best >= 0;
}

static void FlingItem(int i, V3 dir) {
    Il2CppObject *rb = GcGet(st.items[i].rb);
    if (!IsAlive(rb)) return;
    lk.rbSetVel[rb](ToU(dir * kFlingSpeed));
}

// ----------------------------------------------------------------------------
//  Aim dot
// ----------------------------------------------------------------------------
static void ShowDot(V3 p) {
    if (!st.dotBuilt) {
        if (st.dotFailed) return;
        if (!MakePart(st.dot, 0, MkV(0.05f, 0.05f, 0.05f), DotColor(), false)) {
            st.dotFailed = true;
            return;
        }
        st.dotBuilt = true;
        st.dotShown = true;
    }
    Il2CppObject *t = GcGet(st.dot.tr);
    if (!IsAlive(t)) return;
    if (!st.dotShown) { SetPartActive(st.dot, true); st.dotShown = true; }
    lk.setPos[t](ToU(p));
}

static void HideDot() {
    if (st.dotBuilt && st.dotShown) { SetPartActive(st.dot, false); st.dotShown = false; }
}

// ----------------------------------------------------------------------------
//  Menu
// ----------------------------------------------------------------------------
static void Recolor() {
    for (int i = 0; i < 2; i++) {
        Il2CppObject *m = GcGet(st.btn[i].mat);
        if (IsAlive(m)) lk.setColor[m](st.mode == i + 1 ? OnColor() : OffColor());
    }
}

static void SetMode(int m) {
    st.mode = (st.mode == m) ? 0 : m;       // pressing the active button switches it off
    st.cool = 0.3f;                          // don't fire the moment it's switched on
    Recolor();
    if (st.mode == 0) HideDot();
}

#if TWO_MODS_LABELS
static bool MakeLabel(Part &p, const char *text) {
    if (!lk.labelsOk) return false;
    Il2CppObject *go = Class("UnityEngine", "GameObject").CreateNewObjectParameters();  // empty GameObject
    if (!go) return false;
    Il2CppObject *tm = lk.addComponent[go](lk.tmType);
    Il2CppObject *tr = lk.getTransform[go]();
    if (!tm || !tr) { DestroyObj(go); return false; }
    lk.tmText[tm]((Il2CppObject *)CreateMonoString(text));
    lk.tmCharSize[tm](kLabelCharSize);
    lk.tmFontSize[tm](kLabelFontSize);
    lk.tmAnchor[tm](4);                       // TextAnchor.MiddleCenter
    lk.tmAlign[tm](1);                        // TextAlignment.Center
    lk.tmColor[tm](Color(1.0f, 1.0f, 1.0f, 1.0f));
    lk.tmFont[tm](GcGet(lk.font));
    Il2CppObject *ren = nullptr;
    if (lk.tryGet[go](lk.renType, &ren) && ren) lk.renSetMat[ren](GcGet(lk.fontMat));
    p.go = GcHold(go);
    p.tr = GcHold(tr);
    return p.go && p.tr;
}
#endif

static void FreeMenu() {
    for (int i = 0; i < 2; i++) { FreePart(st.btn[i]); FreePart(st.label[i]); }
    st.menuBuilt = false;
    st.menuShown = false;
}

static void BuildMenu() {
    bool ok = true;
    for (int i = 0; i < 2; i++)
        ok = MakePart(st.btn[i], 3, MkV(kBtnW, kBtnH, 0.012f), OffColor(), false) && ok;
#if TWO_MODS_LABELS
    if (ok && lk.labelsOk) {
        MakeLabel(st.label[0], "Fling Item Gun");       // a missing label is fine, the button still works
        MakeLabel(st.label[1], "Item Spawner Gun");
    }
#endif
    if (!ok) {
        ym::log("[2 mods] could not build the menu");
        FreeMenu();
        st.menuFailed = true;
        return;
    }
    st.menuBuilt = true;
    st.menuShown = true;
    Recolor();
}

static void HideMenu() {
    for (int i = 0; i < 2; i++) { SetPartActive(st.btn[i], false); SetPartActive(st.label[i], false); }
    st.menuShown = false;
}

static void ShowMenu() {
    if (st.menuShown) return;
    for (int i = 0; i < 2; i++) { SetPartActive(st.btn[i], true); SetPartActive(st.label[i], true); }
    st.menuShown = true;
}

static void MenuTick(float dt, Il2CppObject *player, Il2CppObject *rightHand) {
    Il2CppObject *lh = lk.fLeft[player]();
    Il2CppObject *head = lk.fHead[player]();
    if (!IsAlive(lh) || !IsAlive(head)) return;

    V3 center = PosOf(lh) + MkV(0, kMenuLift, 0);
    V3 f = center - PosOf(head);
    if (Len(f) < 0.05f) return;
    f = Norm(f);                                    // points away from your face
    V3 r = Cross(MkV(0, 1, 0), f);
    if (Len(r) < 0.01f) return;                     // looking straight up/down: skip this frame
    r = Norm(r);
    V3 u = Cross(f, r);

    ShowMenu();
    Quaternion q = lk.lookRot(ToU(f), ToU(u));
    for (int i = 0; i < 2; i++) {
        V3 p = center + u * kBtnY[i];
        PlacePart(st.btn[i], p, q);
        PlacePart(st.label[i], p - f * 0.012f, q);  // text sits on the side facing you
    }

    // Touch detection: right fingertip vs the two button rectangles
    st.press -= dt;
    if (st.press > 0) return;
    V3 d = (PosOf(rightHand) + AimOf(rightHand) * kFingerOffset) - center;
    float dx = Dot(d, r), dy = Dot(d, u), dz = Dot(d, f);
    if (std::fabs(dz) > kPressDepth || std::fabs(dx) > kBtnW * 0.5f) return;
    for (int i = 0; i < 2; i++) {
        if (std::fabs(dy - kBtnY[i]) < kBtnH * 0.5f + 0.01f) {
            SetMode(i + 1);
            st.press = kPressDelay;
            return;
        }
    }
}

// ----------------------------------------------------------------------------
//  The guns
// ----------------------------------------------------------------------------
static void GunTick(float dt, Il2CppObject *rightHand) {
    st.cool -= dt;
    if (!ReadBtn(5, st.sGrip)) { HideDot(); return; }     // hold the right grip to aim

    V3 o = PosOf(rightHand);
    V3 d = AimOf(rightHand);
    bool fire = ReadBtn(5, st.sTrigger);

    if (st.mode == 2) {                                   // Item Spawner Gun
        V3 spot = o + d * kSpawnDist;
        ShowDot(spot);
        if (fire && st.cool <= 0) { SpawnItem(spot); st.cool = kSpawnDelay; }
    } else {                                              // Fling Item Gun
        int best;
        V3 hitPos = o;
        bool hit = AimAtItem(o, d, best, hitPos);
        ShowDot(hit ? hitPos : o + d * 3.0f);
        if (fire && hit && st.cool <= 0) { FlingItem(best, d); st.cool = kFlingDelay; }
    }
}

// ----------------------------------------------------------------------------
//  Room changes / main loop
// ----------------------------------------------------------------------------
static void ResetWorld() {
    for (int i = 0; i < kMaxItems; i++) FreeItem(st.items[i]);
    FreeMenu();
    FreePart(st.dot);
    st.dotBuilt = st.dotShown = st.dotFailed = false;
    st.menuFailed = false;
    st.nextSlot = 0;
    st.cool = 0;
    st.press = 0.4f;
}

static void OnRoomChange() { st.dirty = true; }

static void Tick(float dt) {
    if (!EnsureLookups()) return;

    Il2CppObject *player = (Il2CppObject *)ym::api()->localPlayer();
    if (!player) return;
    if (player != st.player || st.dirty) {                // new room: every old object is gone
        ResetWorld();
        st.player = player;
        st.dirty = false;
    }

    bool menuKey = ReadBtn(4, st.sSecondary);             // Y on the left controller
    if (!menuKey && !st.menuShown && st.mode == 0) return; // nothing on: do almost nothing

    Il2CppObject *rh = lk.fRight[player]();
    if (!IsAlive(rh)) return;

    if (menuKey) {
        HideDot();
        if (!st.menuBuilt && !st.menuFailed) BuildMenu();
        if (st.menuBuilt) MenuTick(dt, player, rh);
        return;                                           // no firing while the menu is open
    }
    if (st.menuShown) HideMenu();
    if (st.mode != 0) GunTick(dt, rh);
}

static void OnUpdate(float dt) {
    try {
        Tick(dt);
    } catch (...) {
    }
}

YM_EXPORT int YeepsMod_Init(const YeepsModApi *api) {
    if (!ym::init(api)) return -1;
    ym::onUpdate(OnUpdate);
    ym::onRoomChange(OnRoomChange);
    return 0;
}


