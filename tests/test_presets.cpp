// ---------------------------------------------------------------------------
// test_presets.cpp - the HDR preset table: built-in order and values, the
// mkvmerge colour flags derived from a preset, the transfer-kind helpers and
// the user-preset JSON round trip through presets.json.
// ---------------------------------------------------------------------------
#include "test_framework.h"

#include "core/HdrPresets.h"
#include "core/JobModel.h"

#include <cstddef>
#include <string>
#include <vector>

using hh::HdrPreset;
using hh::PresetRegistry;
using hh::TransferKind;

namespace {

/// The built-in ids in the order the registry must present them: HLG family,
/// PQ family, SDR, then the two sentinels.
const std::vector<std::wstring>& expectedBuiltInIds() {
    static const std::vector<std::wstring> ids = {
        L"dji_pocket3_hlg", L"dji_mavic_hlg", L"sony_hlg", L"panasonic_hlg", L"generic_hlg",
        L"iphone_pq", L"sony_hdr10_pq", L"generic_pq_1000", L"generic_pq_4000",
        L"sdr_rec709",
        L"custom", L"source",
    };
    return ids;
}

/// The five HLG ids (no mastering-display metadata on any of them).
const std::vector<std::wstring>& hlgIds() {
    static const std::vector<std::wstring> ids = {
        L"dji_pocket3_hlg", L"dji_mavic_hlg", L"sony_hlg", L"panasonic_hlg", L"generic_hlg",
    };
    return ids;
}

/**
 * @brief Element-by-element comparison of two flag lists so a mismatch
 *        reports the offending value instead of an opaque "<value>".
 */
void checkFlagsEqual(const std::vector<std::wstring>& actual, const std::vector<std::wstring>& expected) {
    if (!CHECK_EQ(actual.size(), expected.size())) { return; }
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK_WEQ(actual[i], expected[i]);
    }
}

/// True when every mastering-display field of @p p is empty.
bool masteringFieldsEmpty(const HdrPreset& p) {
    return p.maxCll.empty() && p.maxFall.empty() && p.chromaticity.empty() && p.whitePoint.empty() &&
           p.maxLuminance.empty() && p.minLuminance.empty();
}

/// A fully populated PQ user preset for the JSON tests.
HdrPreset userPq() {
    HdrPreset p;
    p.id = L"my_pq_2000";
    p.label = L"My PQ 2000 é";   // non-ASCII on purpose: UTF-8 round trip
    p.family = HdrPreset::Family::PQ;
    p.matrix = L"9";
    p.range = L"1";
    p.transfer = L"16";
    p.primaries = L"9";
    p.maxCll = L"2000";
    p.maxFall = L"600";
    p.chromaticity = L"0.680,0.320,0.265,0.690,0.150,0.060";
    p.whitePoint = L"0.3127,0.3290";
    p.maxLuminance = L"2000";
    p.minLuminance = L"0.005";
    p.builtIn = false;
    return p;
}

/// A minimal HLG user preset (no mastering fields).
HdrPreset userHlg() {
    HdrPreset p;
    p.id = L"my_hlg";
    p.label = L"My HLG";
    p.family = HdrPreset::Family::HLG;
    p.matrix = L"9";
    p.range = L"1";
    p.transfer = L"18";
    p.primaries = L"9";
    p.builtIn = false;
    return p;
}

} // namespace

// ---- built-in table --------------------------------------------------------------

/**
 * @brief The built-in table holds exactly the documented ids, in display
 *        order, with "custom" and "source" as the two trailing sentinels.
 */
HH_TEST(Presets_builtInOrder) {
    const PresetRegistry reg;
    const std::vector<HdrPreset>& list = reg.builtIn();
    const std::vector<std::wstring>& ids = expectedBuiltInIds();
    if (!CHECK_EQ(list.size(), ids.size())) { return; }

    // Ids in order; every entry is flagged built-in and has a label.
    for (size_t i = 0; i < ids.size(); ++i) {
        CHECK_WEQ(list[i].id, ids[i]);
        CHECK(list[i].builtIn);
        CHECK_FALSE(list[i].label.empty());
    }

    // Only the last two are sentinels.
    for (size_t i = 0; i < list.size(); ++i) {
        CHECK_EQ(list[i].isSentinel(), i >= list.size() - 2);
    }
    CHECK_EQ(list[list.size() - 2].family, HdrPreset::Family::Custom);
    CHECK_EQ(list.back().family, HdrPreset::Family::Source);

    // No user presets on a fresh registry; all() is just the built-ins.
    CHECK(reg.user().empty());
    CHECK_EQ(reg.all().size(), list.size());
}

/**
 * @brief find() is id-based and case-insensitive; unknown or empty ids
 *        give nullptr.
 */
HH_TEST(Presets_findById) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"generic_pq_1000");
    if (CHECK(p != nullptr)) {
        CHECK_WEQ(p->id, L"generic_pq_1000");
        // Same entry regardless of case or surrounding whitespace.
        CHECK(reg.find(L"GENERIC_PQ_1000") == p);
        CHECK(reg.find(L"  generic_pq_1000 ") == p);
    }
    CHECK(reg.find(L"") == nullptr);
    CHECK(reg.find(L"does_not_exist") == nullptr);

    // Labels resolve too (legacy settings stored the label).
    const HdrPreset* byLabel = reg.findByLabel(L"Generic Rec.2100 PQ / HDR10 (1000 nits)");
    CHECK(byLabel == p);
}

/**
 * @brief generic_pq_1000 carries the full HDR10 mastering set for a
 *        1000-nit display.
 */
HH_TEST(Presets_genericPq1000Values) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"generic_pq_1000");
    if (!CHECK(p != nullptr)) { return; }
    CHECK_EQ(p->family, HdrPreset::Family::PQ);
    CHECK_WEQ(p->matrix, L"9");
    CHECK_WEQ(p->range, L"1");
    CHECK_WEQ(p->transfer, L"16");
    CHECK_WEQ(p->primaries, L"9");
    CHECK_WEQ(p->maxCll, L"1000");
    CHECK_WEQ(p->maxFall, L"400");
    CHECK_WEQ(p->chromaticity, L"0.708,0.292,0.170,0.797,0.131,0.046");
    CHECK_WEQ(p->whitePoint, L"0.3127,0.329");
    CHECK_WEQ(p->maxLuminance, L"1000");
    CHECK_WEQ(p->minLuminance, L"0.0001");
    CHECK(p->builtIn);
    CHECK_FALSE(p->isSentinel());
}

/**
 * @brief generic_pq_4000 differs from the 1000-nit preset only in the
 *        light-level and peak-luminance fields.
 */
HH_TEST(Presets_genericPq4000Values) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"generic_pq_4000");
    if (!CHECK(p != nullptr)) { return; }
    CHECK_EQ(p->family, HdrPreset::Family::PQ);
    CHECK_WEQ(p->matrix, L"9");
    CHECK_WEQ(p->range, L"1");
    CHECK_WEQ(p->transfer, L"16");
    CHECK_WEQ(p->primaries, L"9");
    CHECK_WEQ(p->maxCll, L"4000");
    CHECK_WEQ(p->maxFall, L"1000");
    CHECK_WEQ(p->chromaticity, L"0.708,0.292,0.170,0.797,0.131,0.046");
    CHECK_WEQ(p->whitePoint, L"0.3127,0.329");
    CHECK_WEQ(p->maxLuminance, L"4000");
    CHECK_WEQ(p->minLuminance, L"0.0001");

    // The two generic PQ presets are distinct by value, not just by id.
    const HdrPreset* thousand = reg.find(L"generic_pq_1000");
    if (CHECK(thousand != nullptr)) {
        CHECK_FALSE(p->sameValuesAs(*thousand));
    }
}

/**
 * @brief Every HLG preset uses transfer 18 with Rec.2020 matrix/primaries
 *        and no mastering-display metadata at all.
 */
HH_TEST(Presets_hlgFamilyHasNoMasteringMetadata) {
    const PresetRegistry reg;
    for (const std::wstring& id : hlgIds()) {
        const HdrPreset* p = reg.find(id);
        if (!CHECK(p != nullptr)) { continue; }
        CHECK_EQ(p->family, HdrPreset::Family::HLG);
        CHECK_WEQ(p->matrix, L"9");
        CHECK_WEQ(p->range, L"1");
        CHECK_WEQ(p->transfer, L"18");
        CHECK_WEQ(p->primaries, L"9");
        CHECK(masteringFieldsEmpty(*p));
    }
}

/**
 * @brief sdr_rec709 is all ones (BT.709 matrix, limited range, BT.709
 *        transfer and primaries) and carries no HDR metadata.
 */
HH_TEST(Presets_sdrRec709IsAllOnes) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"sdr_rec709");
    if (!CHECK(p != nullptr)) { return; }
    CHECK_EQ(p->family, HdrPreset::Family::SDR);
    CHECK_WEQ(p->matrix, L"1");
    CHECK_WEQ(p->range, L"1");
    CHECK_WEQ(p->transfer, L"1");
    CHECK_WEQ(p->primaries, L"1");
    CHECK(masteringFieldsEmpty(*p));
}

// ---- colour flags ----------------------------------------------------------------

/**
 * @brief A PQ preset on track 0 emits the full 20-element mkvmerge flag
 *        list in the exact order the Python tool used.
 */
HH_TEST(Presets_colourFlagsPqTrackZero) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"generic_pq_1000");
    if (!CHECK(p != nullptr)) { return; }

    const std::vector<std::wstring> expected = {
        L"--color-matrix-coefficients", L"0:9",
        L"--color-range", L"0:1",
        L"--color-transfer-characteristics", L"0:16",
        L"--color-primaries", L"0:9",
        L"--max-content-light", L"0:1000",
        L"--max-frame-light", L"0:400",
        L"--chromaticity-coordinates", L"0:0.708,0.292,0.170,0.797,0.131,0.046",
        L"--white-color-coordinates", L"0:0.3127,0.329",
        L"--max-luminance", L"0:1000",
        L"--min-luminance", L"0:0.0001",
    };
    checkFlagsEqual(PresetRegistry::colourFlags(*p, 0), expected);
}

/**
 * @brief An HLG preset on track 1 emits only the four enum flags; empty
 *        mastering fields produce nothing.
 */
HH_TEST(Presets_colourFlagsHlgTrackOne) {
    const PresetRegistry reg;
    const HdrPreset* p = reg.find(L"generic_hlg");
    if (!CHECK(p != nullptr)) { return; }

    const std::vector<std::wstring> expected = {
        L"--color-matrix-coefficients", L"1:9",
        L"--color-range", L"1:1",
        L"--color-transfer-characteristics", L"1:18",
        L"--color-primaries", L"1:9",
    };
    checkFlagsEqual(PresetRegistry::colourFlags(*p, 1), expected);
}

/**
 * @brief Sentinels and all-empty presets emit no flags at all.
 */
HH_TEST(Presets_colourFlagsSentinelsAreEmpty) {
    const PresetRegistry reg;
    for (const wchar_t* id : {L"custom", L"source"}) {
        const HdrPreset* p = reg.find(id);
        if (!CHECK(p != nullptr)) { continue; }
        CHECK(p->isSentinel());
        CHECK(PresetRegistry::colourFlags(*p, 0).empty());
    }

    // A non-sentinel with nothing filled in is equally silent.
    HdrPreset blank;
    blank.family = HdrPreset::Family::PQ;
    CHECK(PresetRegistry::colourFlags(blank, 0).empty());

    // A partially filled preset only emits what it has.
    HdrPreset partial;
    partial.family = HdrPreset::Family::PQ;
    partial.transfer = L"16";
    partial.maxCll = L" 1000 ";   // whitespace is trimmed, not emitted
    checkFlagsEqual(PresetRegistry::colourFlags(partial, 2),
                    {L"--color-transfer-characteristics", L"2:16", L"--max-content-light", L"2:1000"});
}

// ---- transfer helpers ------------------------------------------------------------

/**
 * @brief transferKind() reads the transfer code: 16 is PQ, 18 is HLG, 1 is
 *        SDR; sentinels report Unknown.
 */
HH_TEST(Presets_transferKindFromTransferCode) {
    const PresetRegistry reg;
    const HdrPreset* pq = reg.find(L"generic_pq_1000");
    const HdrPreset* hlg = reg.find(L"generic_hlg");
    const HdrPreset* sdr = reg.find(L"sdr_rec709");
    const HdrPreset* custom = reg.find(L"custom");
    const HdrPreset* source = reg.find(L"source");
    if (CHECK(pq != nullptr)) { CHECK_EQ(pq->transferKind(), TransferKind::PQ); }
    if (CHECK(hlg != nullptr)) { CHECK_EQ(hlg->transferKind(), TransferKind::HLG); }
    if (CHECK(sdr != nullptr)) { CHECK_EQ(sdr->transferKind(), TransferKind::SDR); }
    if (CHECK(custom != nullptr)) { CHECK_EQ(custom->transferKind(), TransferKind::Unknown); }
    if (CHECK(source != nullptr)) { CHECK_EQ(source->transferKind(), TransferKind::Unknown); }

    // An unrecognised code on a plain preset is Unknown as well.
    HdrPreset odd;
    odd.family = HdrPreset::Family::PQ;
    odd.transfer = L"14";
    CHECK_EQ(odd.transferKind(), TransferKind::Unknown);
}

/**
 * @brief defaultIdFor() maps each transfer to the generic preset of its
 *        family; Unknown has no default.
 */
HH_TEST(Presets_defaultIdFor) {
    CHECK_WEQ(PresetRegistry::defaultIdFor(TransferKind::PQ), L"generic_pq_1000");
    CHECK_WEQ(PresetRegistry::defaultIdFor(TransferKind::HLG), L"generic_hlg");
    CHECK_WEQ(PresetRegistry::defaultIdFor(TransferKind::SDR), L"sdr_rec709");
    CHECK(PresetRegistry::defaultIdFor(TransferKind::Unknown).empty());

    // Every default resolves to a real built-in of the matching family.
    const PresetRegistry reg;
    const HdrPreset* pq = reg.find(PresetRegistry::defaultIdFor(TransferKind::PQ));
    const HdrPreset* hlg = reg.find(PresetRegistry::defaultIdFor(TransferKind::HLG));
    const HdrPreset* sdr = reg.find(PresetRegistry::defaultIdFor(TransferKind::SDR));
    if (CHECK(pq != nullptr)) { CHECK_EQ(pq->family, HdrPreset::Family::PQ); }
    if (CHECK(hlg != nullptr)) { CHECK_EQ(hlg->family, HdrPreset::Family::HLG); }
    if (CHECK(sdr != nullptr)) { CHECK_EQ(sdr->family, HdrPreset::Family::SDR); }
}

/**
 * @brief forTransfer() returns only the presets of that family and never a
 *        sentinel.
 */
HH_TEST(Presets_forTransferFiltersByFamily) {
    const PresetRegistry reg;

    // PQ: the four PQ presets, nothing else.
    const std::vector<HdrPreset> pq = reg.forTransfer(TransferKind::PQ);
    CHECK_EQ(pq.size(), 4);
    for (const HdrPreset& p : pq) {
        CHECK_EQ(p.family, HdrPreset::Family::PQ);
        CHECK_WEQ(p.transfer, L"16");
        CHECK_FALSE(p.isSentinel());
    }
    if (pq.size() == 4) {
        CHECK_WEQ(pq[0].id, L"iphone_pq");
        CHECK_WEQ(pq[1].id, L"sony_hdr10_pq");
        CHECK_WEQ(pq[2].id, L"generic_pq_1000");
        CHECK_WEQ(pq[3].id, L"generic_pq_4000");
    }

    // HLG: the five HLG presets.
    const std::vector<HdrPreset> hlg = reg.forTransfer(TransferKind::HLG);
    CHECK_EQ(hlg.size(), hlgIds().size());
    for (const HdrPreset& p : hlg) {
        CHECK_EQ(p.family, HdrPreset::Family::HLG);
        CHECK_FALSE(p.isSentinel());
    }

    // SDR: just the one.
    const std::vector<HdrPreset> sdr = reg.forTransfer(TransferKind::SDR);
    if (CHECK_EQ(sdr.size(), 1)) {
        CHECK_WEQ(sdr[0].id, L"sdr_rec709");
    }

    // Unknown: everything that is not a sentinel.
    const std::vector<HdrPreset> any = reg.forTransfer(TransferKind::Unknown);
    CHECK_EQ(any.size(), reg.builtIn().size() - 2);
    for (const HdrPreset& p : any) {
        CHECK_FALSE(p.isSentinel());
    }
}

// ---- user presets ----------------------------------------------------------------

/**
 * @brief A user preset whose id collides with a built-in is renamed with a
 *        "_user" suffix; the built-in itself is untouched.
 */
HH_TEST(Presets_upsertUserAvoidsBuiltInIds) {
    PresetRegistry reg;
    HdrPreset mine = userHlg();
    mine.id = L"generic_hlg";
    mine.label = L"Mine";
    reg.upsertUser(mine);

    if (CHECK_EQ(reg.user().size(), 1)) {
        const HdrPreset& stored = reg.user()[0];
        CHECK_WEQ(stored.id, L"generic_hlg_user");
        CHECK_WEQ(stored.label, L"Mine");
        CHECK_FALSE(stored.builtIn);
        CHECK_EQ(stored.family, HdrPreset::Family::HLG);
    }

    // The built-in still answers to its own id and keeps its label.
    const HdrPreset* builtIn = reg.find(L"generic_hlg");
    if (CHECK(builtIn != nullptr)) {
        CHECK(builtIn->builtIn);
        CHECK_WEQ(builtIn->label, L"Generic Rec.2100 HLG");
    }

    // The renamed one is reachable and listed after the built-ins.
    const HdrPreset* renamed = reg.find(L"generic_hlg_user");
    if (CHECK(renamed != nullptr)) {
        CHECK_FALSE(renamed->builtIn);
    }
    const std::vector<HdrPreset> all = reg.all();
    if (CHECK_EQ(all.size(), reg.builtIn().size() + 1)) {
        CHECK_WEQ(all.back().id, L"generic_hlg_user");
    }

    // Upserting the same (renamed) id again replaces rather than duplicates.
    HdrPreset again = userHlg();
    again.id = L"generic_hlg_user";
    again.label = L"Mine v2";
    reg.upsertUser(again);
    if (CHECK_EQ(reg.user().size(), 1)) {
        CHECK_WEQ(reg.user()[0].label, L"Mine v2");
    }

    // A preset without an id gets one derived from its label.
    HdrPreset unnamed = userPq();
    unnamed.id.clear();
    unnamed.label = L"Studio PQ (Bright)";
    reg.upsertUser(unnamed);
    CHECK(reg.find(L"studio_pq_bright") != nullptr);

    // removeUser only removes user presets.
    CHECK(reg.removeUser(L"generic_hlg_user"));
    CHECK_FALSE(reg.removeUser(L"generic_hlg_user"));
    CHECK_FALSE(reg.removeUser(L"generic_hlg"));
    CHECK(reg.find(L"generic_hlg") != nullptr);
}

/**
 * @brief loadUser() on a path that does not exist is a success with no
 *        user presets; an empty path is refused.
 */
HH_TEST(Presets_loadUserMissingFileIsOk) {
    hh::test::ScratchDir dir(L"presets_missing");
    if (!CHECK(dir.valid())) { return; }

    PresetRegistry reg;
    reg.upsertUser(userHlg());   // must be cleared by the load
    const auto r = reg.loadUser(dir.file(L"does_not_exist.json"));
    CHECK(r.ok());
    CHECK(reg.user().empty());
    CHECK_EQ(reg.builtIn().size(), expectedBuiltInIds().size());

    CHECK_FALSE(reg.loadUser(L"").ok());
    CHECK_FALSE(reg.loadUser(L"   ").ok());
}

/**
 * @brief saveUser() then loadUser() through a temp file under
 *        %TEMP%\hdrhint_tests brings back every field of every user preset.
 */
HH_TEST(Presets_saveLoadUserRoundTrip) {
    // ScratchDir lives under platform::tempFolder() + "\hdrhint_tests" and
    // removes itself when the test ends.
    hh::test::ScratchDir dir(L"presets_roundtrip");
    if (!CHECK(dir.valid())) { return; }
    const std::wstring path = dir.file(L"presets.json");

    const HdrPreset pq = userPq();
    const HdrPreset hlg = userHlg();

    // Write two user presets.
    PresetRegistry writer;
    writer.upsertUser(pq);
    writer.upsertUser(hlg);
    CHECK_EQ(writer.user().size(), 2);
    const auto saved = writer.saveUser(path);
    if (!CHECK(saved.ok())) { return; }
    CHECK(dir.exists(L"presets.json"));
    CHECK_FALSE(dir.readFile(L"presets.json").empty());

    // Read them back into a fresh registry.
    PresetRegistry reader;
    const auto loaded = reader.loadUser(path);
    if (!CHECK(loaded.ok())) { return; }
    if (!CHECK_EQ(reader.user().size(), 2)) { return; }

    // Order and identity survive.
    CHECK_WEQ(reader.user()[0].id, pq.id);
    CHECK_WEQ(reader.user()[1].id, hlg.id);

    // The PQ preset: every field, the label's non-ASCII included.
    const HdrPreset* a = reader.find(pq.id);
    if (CHECK(a != nullptr)) {
        CHECK_WEQ(a->label, pq.label);
        CHECK_EQ(a->family, HdrPreset::Family::PQ);
        CHECK_FALSE(a->builtIn);
        CHECK(a->sameValuesAs(pq));
        CHECK_WEQ(a->matrix, pq.matrix);
        CHECK_WEQ(a->range, pq.range);
        CHECK_WEQ(a->transfer, pq.transfer);
        CHECK_WEQ(a->primaries, pq.primaries);
        CHECK_WEQ(a->maxCll, pq.maxCll);
        CHECK_WEQ(a->maxFall, pq.maxFall);
        CHECK_WEQ(a->chromaticity, pq.chromaticity);
        CHECK_WEQ(a->whitePoint, pq.whitePoint);
        CHECK_WEQ(a->maxLuminance, pq.maxLuminance);
        CHECK_WEQ(a->minLuminance, pq.minLuminance);
        CHECK_EQ(a->transferKind(), TransferKind::PQ);
    }

    // The HLG preset: empty mastering fields stay empty.
    const HdrPreset* b = reader.find(hlg.id);
    if (CHECK(b != nullptr)) {
        CHECK_WEQ(b->label, hlg.label);
        CHECK_EQ(b->family, HdrPreset::Family::HLG);
        CHECK_FALSE(b->builtIn);
        CHECK(b->sameValuesAs(hlg));
        CHECK(masteringFieldsEmpty(*b));
        CHECK_EQ(b->transferKind(), TransferKind::HLG);
    }

    // Built-ins are unaffected and user presets appear after them.
    CHECK_EQ(reader.builtIn().size(), writer.builtIn().size());
    CHECK_EQ(reader.all().size(), reader.builtIn().size() + 2);
    CHECK_EQ(reader.forTransfer(TransferKind::PQ).size(), 5);
    CHECK_EQ(reader.forTransfer(TransferKind::HLG).size(), hlgIds().size() + 1);

    // Saving again from the reader is byte-for-byte stable.
    const std::string firstText = dir.readFile(L"presets.json");
    CHECK(reader.saveUser(path).ok());
    CHECK_EQ(dir.readFile(L"presets.json"), firstText);
}
