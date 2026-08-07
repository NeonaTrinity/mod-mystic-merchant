/*
 * mod-mystic-merchant
 * AzerothCore 3.3.5a module
 *
 * Copyright (C) 2026 Trinity / Neona
 * Released under the GNU AGPL v3 or later.
 */

#include "Bag.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "ScriptedGossip.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace MysticMerchant
{
constexpr uint32 COPPER_PER_GOLD = 10000;
constexpr uint32 DEFAULT_NPC_ENTRY = 900201;
constexpr uint32 GOSSIP_TEXT_ID = 1;

// Consolation rewards used when a gray/white quality roll has no valid
// equipment item for the selected category, slot, and level bracket.
constexpr uint32 FALLBACK_GRAY_BUTTON = 43330;       // Broken U.L.O.S.E. Button
constexpr uint32 FALLBACK_GRAY_BOAR_TUSK = 3171;    // Broken Boar Tusk
constexpr uint32 FALLBACK_GRAY_WAND = 3769;         // Broken Wand
constexpr uint32 FALLBACK_WHITE_ARMOR_SCRAPS = 17422;
constexpr uint32 FALLBACK_WHITE_LEATHER_SCRAPS = 2934;

// Item subclass values from ItemSubClass.dbc / SharedDefines.
constexpr uint32 ARMOR_CLOTH = 1;
constexpr uint32 ARMOR_LEATHER = 2;
constexpr uint32 ARMOR_MAIL = 3;
constexpr uint32 ARMOR_PLATE = 4;
constexpr uint32 ARMOR_SHIELD = 6;
constexpr uint32 ARMOR_LIBRAM = 7;
constexpr uint32 ARMOR_IDOL = 8;
constexpr uint32 ARMOR_TOTEM = 9;
constexpr uint32 ARMOR_SIGIL = 10;

constexpr uint32 WEAPON_AXE_1H = 0;
constexpr uint32 WEAPON_AXE_2H = 1;
constexpr uint32 WEAPON_BOW = 2;
constexpr uint32 WEAPON_GUN = 3;
constexpr uint32 WEAPON_MACE_1H = 4;
constexpr uint32 WEAPON_MACE_2H = 5;
constexpr uint32 WEAPON_POLEARM = 6;
constexpr uint32 WEAPON_SWORD_1H = 7;
constexpr uint32 WEAPON_SWORD_2H = 8;
constexpr uint32 WEAPON_STAFF = 10;
constexpr uint32 WEAPON_FIST = 13;
constexpr uint32 WEAPON_DAGGER = 15;
constexpr uint32 WEAPON_THROWN = 16;
constexpr uint32 WEAPON_CROSSBOW = 18;
constexpr uint32 WEAPON_WAND = 19;
constexpr uint32 WEAPON_FISHING_POLE = 20;

enum class SelectionKind : uint8
{
    None,
    Armor,
    Accessory,
    OffHand,
    Weapon
};

enum class SlotChoice : uint8
{
    None,
    Head,
    Shoulders,
    Chest,
    Wrists,
    Hands,
    Waist,
    Legs,
    Feet,
    Back,
    Neck,
    Finger,
    Trinket,
    Shield,
    HeldInOffHand,
    Relic
};

enum GossipAction : uint32
{
    ACTION_MAIN = 1,
    ACTION_OPEN_CHESTS = 10,
    ACTION_GAMBLE_ARMOR = 20,
    ACTION_GAMBLE_ACCESSORIES = 21,
    ACTION_GAMBLE_OFFHANDS = 22,
    ACTION_GAMBLE_WEAPONS = 30,
    ACTION_EXPLAIN = 40,
    ACTION_CLOSE = 50,

    ACTION_ARMOR_BASE = 100,
    ACTION_SLOT_BASE = 200,
    ACTION_WEAPON_BASE = 300,
    ACTION_LEVEL_BASE = 500,

    ACTION_CHEST_ENTRY_BASE = 1000,
    ACTION_CHEST_OPEN_ONE = 2000,
    ACTION_CHEST_OPEN_ALL = 2001,
    ACTION_CHEST_BACK = 2002
};

struct LevelBracket
{
    uint8 minLevel;
    uint8 maxLevel;
    uint8 requiredExpansion;
    uint32 defaultPriceGold;
    uint32 maxItemLevel;
    char const* configKey;
    char const* label;
};

std::array<LevelBracket, 10> const Brackets = {{
    {10, 19, 0,   5,  29, "10_19", "Level 10-19"},
    {20, 29, 0,  10,  39, "20_29", "Level 20-29"},
    {30, 39, 0,  20,  49, "30_39", "Level 30-39"},
    {40, 49, 0,  40,  59, "40_49", "Level 40-49"},
    {50, 59, 0,  50,  69, "50_59", "Level 50-59"},
    {60, 60, 0, 100,  75, "60",    "Level 60"},
    {61, 69, 1, 150, 115, "61_69", "Level 61-69"},
    {70, 70, 1, 250, 125, "70",    "Level 70"},
    {71, 79, 2, 300, 187, "71_79", "Level 71-79"},
    {80, 80, 2, 500, 200, "80",    "Level 80"}
}};

struct ConfigData
{
    bool enabled = true;
    uint32 npcEntry = DEFAULT_NPC_ENTRY;
    uint8 expansion = 2;

    bool chestEnabled = true;
    uint8 chestMode = 0; // 0 native loot, 1 replacement equipment
    bool chestOpenOne = true;
    bool chestOpenAll = true;
    uint32 chestOpenAllMax = 20;
    uint32 chestMinimumFreeSlots = 6;

    bool gamblingEnabled = true;
    bool requirePlayerLevel = true;
    uint32 grayChance = 0;
    uint32 whiteChance = 0;
    uint32 greenChance = 80;
    uint32 blueChance = 18;
    uint32 epicChance = 2;
    bool useLootSourcesOnly = true;
    bool allowWorldContainersInPool = false;
    bool allowSets = false;
    bool allowReputationItems = false;
    bool allowQuestItems = false;
    bool allowConjuredItems = false;
    bool logTransactions = true;

    std::array<uint32, Brackets.size()> pricesGold{};
    std::array<uint32, Brackets.size()> maxItemLevels{};
};

struct Candidate
{
    uint32 entry = 0;
    uint32 itemClass = 0;
    uint32 subClass = 0;
    uint32 inventoryType = 0;
    uint32 requiredLevel = 0;
    uint32 itemLevel = 0;
    uint32 quality = 0;
};

struct PlayerState
{
    SelectionKind kind = SelectionKind::None;
    uint32 subClass = 0;
    SlotChoice slot = SlotChoice::None;
    uint32 selectedChest = 0;
    std::vector<uint32> chestEntries;
};

ConfigData gConfig;
std::vector<Candidate> gCandidates;
std::map<uint32, PlayerState> gStates;
std::mutex gStateMutex;
std::mutex gPoolMutex;
std::atomic_bool gPoolLoaded{false};

uint32 PlayerKey(Player const* player)
{
    return player->GetGUID().GetCounter();
}

PlayerState& GetState(Player* player)
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    return gStates[PlayerKey(player)];
}

void ClearState(Player const* player)
{
    std::lock_guard<std::mutex> lock(gStateMutex);
    gStates.erase(PlayerKey(player));
}

void SendMessage(Player* player, std::string const& message)
{
    ChatHandler(player->GetSession()).PSendSysMessage("|cff9f70ff[Mystic Merchant]|r {}", message);
}

uint32 GetConfiguredPriceGold(uint32 bracketIndex)
{
    if (bracketIndex >= Brackets.size())
        return 0;
    return gConfig.pricesGold[bracketIndex];
}

void LoadConfig()
{
    gConfig.enabled = sConfigMgr->GetOption<bool>("MysticMerchant.Enable", true);
    gConfig.npcEntry = sConfigMgr->GetOption<uint32>("MysticMerchant.NpcEntry", DEFAULT_NPC_ENTRY);
    gConfig.expansion = static_cast<uint8>(std::min<uint32>(2, sConfigMgr->GetOption<uint32>("MysticMerchant.Expansion", 2)));

    gConfig.chestEnabled = sConfigMgr->GetOption<bool>("MysticMerchant.Chests.Enable", true);
    gConfig.chestMode = static_cast<uint8>(std::min<uint32>(1, sConfigMgr->GetOption<uint32>("MysticMerchant.Chests.Mode", 0)));
    gConfig.chestOpenOne = sConfigMgr->GetOption<bool>("MysticMerchant.Chests.AllowOpenOne", true);
    gConfig.chestOpenAll = sConfigMgr->GetOption<bool>("MysticMerchant.Chests.AllowOpenAllOfType", true);
    gConfig.chestOpenAllMax = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("MysticMerchant.Chests.OpenAllMaximum", 20));
    gConfig.chestMinimumFreeSlots = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("MysticMerchant.Chests.MinimumFreeSlots", 6));

    gConfig.gamblingEnabled = sConfigMgr->GetOption<bool>("MysticMerchant.Gambling.Enable", true);
    gConfig.requirePlayerLevel = sConfigMgr->GetOption<bool>("MysticMerchant.Gambling.RequirePlayerLevel", true);
    gConfig.grayChance = sConfigMgr->GetOption<uint32>("MysticMerchant.Gambling.GrayChance", 0);
    gConfig.whiteChance = sConfigMgr->GetOption<uint32>("MysticMerchant.Gambling.WhiteChance", 0);
    gConfig.greenChance = sConfigMgr->GetOption<uint32>("MysticMerchant.Gambling.GreenChance", 80);
    gConfig.blueChance = sConfigMgr->GetOption<uint32>("MysticMerchant.Gambling.BlueChance", 18);
    gConfig.epicChance = sConfigMgr->GetOption<uint32>("MysticMerchant.Gambling.EpicChance", 2);
    gConfig.useLootSourcesOnly = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.UseLootSourcesOnly", true);
    gConfig.allowWorldContainersInPool = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.IncludeItemLoot", false);
    gConfig.allowSets = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.AllowItemSets", false);
    gConfig.allowReputationItems = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.AllowReputationItems", false);
    gConfig.allowQuestItems = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.AllowQuestItems", false);
    gConfig.allowConjuredItems = sConfigMgr->GetOption<bool>("MysticMerchant.Pool.AllowConjuredItems", false);
    gConfig.logTransactions = sConfigMgr->GetOption<bool>("MysticMerchant.Logging.Enable", true);

    uint64 qualityTotal = static_cast<uint64>(gConfig.grayChance) + gConfig.whiteChance +
        gConfig.greenChance + gConfig.blueChance + gConfig.epicChance;
    if (qualityTotal == 0)
    {
        LOG_WARN("module", "MysticMerchant: all gambling quality weights are 0; using default weights 0/0/80/18/2.");
        gConfig.grayChance = 0;
        gConfig.whiteChance = 0;
        gConfig.greenChance = 80;
        gConfig.blueChance = 18;
        gConfig.epicChance = 2;
    }

    for (uint32 i = 0; i < Brackets.size(); ++i)
    {
        std::string priceKey = "MysticMerchant.Gambling.Price." + std::string(Brackets[i].configKey);
        std::string itemLevelKey = "MysticMerchant.Pool.MaxItemLevel." + std::string(Brackets[i].configKey);
        gConfig.pricesGold[i] = sConfigMgr->GetOption<uint32>(priceKey, Brackets[i].defaultPriceGold);
        gConfig.maxItemLevels[i] = sConfigMgr->GetOption<uint32>(itemLevelKey, Brackets[i].maxItemLevel);
    }
}

bool IsPoolTemplateAllowed(ItemTemplate const* item)
{
    if (!item)
        return false;

    if (item->Class != ITEM_CLASS_ARMOR && item->Class != ITEM_CLASS_WEAPON)
        return false;

    if (item->Quality < ITEM_QUALITY_POOR || item->Quality > ITEM_QUALITY_EPIC)
        return false;

    if (item->RequiredLevel == 0 || item->InventoryType == INVTYPE_NON_EQUIP)
        return false;

    if (!gConfig.allowQuestItems && item->StartQuest != 0)
        return false;

    if (!gConfig.allowSets && item->ItemSet != 0)
        return false;

    if (!gConfig.allowReputationItems && item->RequiredReputationFaction != 0)
        return false;

    if (!gConfig.allowConjuredItems && (item->Flags & ITEM_FLAG_CONJURED))
        return false;

    // Never include obsolete client-facing templates in generated rewards.
    if (item->Flags & ITEM_FLAG_DEPRECATED)
        return false;

    return true;
}

void AddCandidateIfAllowed(uint32 entry, std::unordered_set<uint32>& seen)
{
    if (!entry || !seen.insert(entry).second)
        return;

    ItemTemplate const* item = sObjectMgr->GetItemTemplate(entry);
    if (!IsPoolTemplateAllowed(item))
        return;

    Candidate candidate;
    candidate.entry = entry;
    candidate.itemClass = item->Class;
    candidate.subClass = item->SubClass;
    candidate.inventoryType = item->InventoryType;
    candidate.requiredLevel = item->RequiredLevel;
    candidate.itemLevel = item->ItemLevel;
    candidate.quality = item->Quality;
    gCandidates.push_back(candidate);
}

void LoadRewardPool()
{
    std::lock_guard<std::mutex> lock(gPoolMutex);
    gCandidates.clear();
    std::unordered_set<uint32> seen;

    std::string query;
    if (gConfig.useLootSourcesOnly)
    {
        query =
            "SELECT DISTINCT source.Item FROM ("
            " SELECT Item FROM creature_loot_template WHERE Item > 0 AND Reference = 0"
            " UNION SELECT Item FROM gameobject_loot_template WHERE Item > 0 AND Reference = 0"
            " UNION SELECT Item FROM reference_loot_template WHERE Item > 0 AND Reference = 0";
        if (gConfig.allowWorldContainersInPool)
            query += " UNION SELECT Item FROM item_loot_template WHERE Item > 0 AND Reference = 0";
        query += ") source";
    }
    else
    {
        query = "SELECT entry FROM item_template";
    }

    QueryResult result = WorldDatabase.Query(query);
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            AddCandidateIfAllowed(fields[0].Get<uint32>(), seen);
        } while (result->NextRow());
    }

    gPoolLoaded.store(true);
    LOG_INFO("module", "MysticMerchant: loaded {} eligible equipment entries into the reward pool.", gCandidates.size());
}

void EnsurePoolLoaded()
{
    if (!gPoolLoaded.load())
        LoadRewardPool();
}

uint32 CountItem(Player* player, uint32 entry)
{
    return player->GetItemCount(entry, false);
}

uint32 CountFreeGeneralInventorySlots(Player* player)
{
    uint32 freeSlots = 0;

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++freeSlots;

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = player->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        ItemTemplate const* bagTemplate = bag->GetTemplate();
        if (!bagTemplate || bagTemplate->BagFamily != 0)
            continue;

        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            if (!player->GetItemByPos(bagSlot, slot))
                ++freeSlots;
    }

    return freeSlots;
}

std::map<uint32, uint32> FindLockedContainers(Player* player)
{
    std::map<uint32, uint32> found;

    auto inspect = [&found](Item* item)
    {
        if (!item)
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto || proto->LockID == 0 ||
            (!LootTemplates_Item.HaveLootFor(proto->ItemId) && proto->MaxMoneyLoot == 0))
            return;
        found[proto->ItemId] += item->GetCount();
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        inspect(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = player->GetBagByPos(bagSlot);
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            inspect(player->GetItemByPos(bagSlot, slot));
    }

    return found;
}

int32 FindBracketForLevel(uint32 level)
{
    for (uint32 i = 0; i < Brackets.size(); ++i)
        if (level >= Brackets[i].minLevel && level <= Brackets[i].maxLevel)
            return static_cast<int32>(i);
    return -1;
}

uint32 EstimateContainerLevel(ItemTemplate const* box)
{
    if (!box)
        return 10;
    if (box->RequiredLevel >= 10)
        return box->RequiredLevel;
    if (box->ItemLevel >= 10)
        return std::min<uint32>(80, box->ItemLevel);
    return 10;
}

bool MatchesSlot(Candidate const& candidate, PlayerState const& state)
{
    switch (state.slot)
    {
        case SlotChoice::Head:          return candidate.inventoryType == INVTYPE_HEAD;
        case SlotChoice::Shoulders:     return candidate.inventoryType == INVTYPE_SHOULDERS;
        case SlotChoice::Chest:         return candidate.inventoryType == INVTYPE_CHEST || candidate.inventoryType == INVTYPE_ROBE;
        case SlotChoice::Wrists:        return candidate.inventoryType == INVTYPE_WRISTS;
        case SlotChoice::Hands:         return candidate.inventoryType == INVTYPE_HANDS;
        case SlotChoice::Waist:         return candidate.inventoryType == INVTYPE_WAIST;
        case SlotChoice::Legs:          return candidate.inventoryType == INVTYPE_LEGS;
        case SlotChoice::Feet:          return candidate.inventoryType == INVTYPE_FEET;
        case SlotChoice::Back:          return candidate.inventoryType == INVTYPE_CLOAK;
        case SlotChoice::Neck:          return candidate.inventoryType == INVTYPE_NECK;
        case SlotChoice::Finger:        return candidate.inventoryType == INVTYPE_FINGER;
        case SlotChoice::Trinket:       return candidate.inventoryType == INVTYPE_TRINKET;
        case SlotChoice::Shield:        return candidate.inventoryType == INVTYPE_SHIELD && candidate.subClass == ARMOR_SHIELD;
        case SlotChoice::HeldInOffHand: return candidate.inventoryType == INVTYPE_HOLDABLE;
        case SlotChoice::Relic:         return candidate.inventoryType == INVTYPE_RELIC &&
                                               (candidate.subClass == ARMOR_LIBRAM || candidate.subClass == ARMOR_IDOL ||
                                                candidate.subClass == ARMOR_TOTEM || candidate.subClass == ARMOR_SIGIL);
        default:                        return false;
    }
}

bool MatchesSelection(Candidate const& candidate, PlayerState const& state)
{
    switch (state.kind)
    {
        case SelectionKind::Armor:
            return candidate.itemClass == ITEM_CLASS_ARMOR &&
                   candidate.subClass == state.subClass && MatchesSlot(candidate, state);
        case SelectionKind::Accessory:
            return candidate.itemClass == ITEM_CLASS_ARMOR && MatchesSlot(candidate, state);
        case SelectionKind::OffHand:
            return candidate.itemClass == ITEM_CLASS_ARMOR && MatchesSlot(candidate, state);
        case SelectionKind::Weapon:
            return candidate.itemClass == ITEM_CLASS_WEAPON && candidate.subClass == state.subClass;
        default:
            return false;
    }
}

uint32 RollQuality()
{
    // Treat configured values as relative weights. They do not need to total 100.
    // Example: 10/10/80/18/2 rolls across a total weight of 120.
    uint64 total = static_cast<uint64>(gConfig.grayChance) + gConfig.whiteChance +
        gConfig.greenChance + gConfig.blueChance + gConfig.epicChance;

    // LoadConfig converts an all-zero configuration back to defaults, but keep
    // this guard so a future runtime change can never produce an invalid roll.
    if (total == 0)
        return ITEM_QUALITY_UNCOMMON;

    // Config values are expected to be small weights/percent-like numbers.
    // If an extreme configuration exceeds urand's uint32 range, scale all
    // weights proportionally before rolling instead of disabling gambling.
    uint32 gray = gConfig.grayChance;
    uint32 white = gConfig.whiteChance;
    uint32 green = gConfig.greenChance;
    uint32 blue = gConfig.blueChance;
    uint32 epic = gConfig.epicChance;

    if (total > std::numeric_limits<uint32>::max())
    {
        double scale = static_cast<double>(std::numeric_limits<uint32>::max()) / static_cast<double>(total);
        gray = static_cast<uint32>(gray * scale);
        white = static_cast<uint32>(white * scale);
        green = static_cast<uint32>(green * scale);
        blue = static_cast<uint32>(blue * scale);
        epic = static_cast<uint32>(epic * scale);
        total = static_cast<uint64>(gray) + white + green + blue + epic;

        if (total == 0)
            return ITEM_QUALITY_UNCOMMON;
    }

    uint32 roll = urand(1, static_cast<uint32>(total));
    uint32 cumulative = gray;
    if (roll <= cumulative)
        return ITEM_QUALITY_POOR;

    cumulative += white;
    if (roll <= cumulative)
        return ITEM_QUALITY_NORMAL;

    cumulative += green;
    if (roll <= cumulative)
        return ITEM_QUALITY_UNCOMMON;

    cumulative += blue;
    if (roll <= cumulative)
        return ITEM_QUALITY_RARE;

    return ITEM_QUALITY_EPIC;
}

std::vector<uint32> FindRewardEntries(PlayerState const* selection, uint32 bracketIndex, uint32 quality, bool anyEquipment)
{
    std::vector<uint32> entries;
    if (bracketIndex >= Brackets.size())
        return entries;

    LevelBracket const& bracket = Brackets[bracketIndex];
    uint32 maxItemLevel = gConfig.maxItemLevels[bracketIndex];

    std::lock_guard<std::mutex> lock(gPoolMutex);
    for (Candidate const& candidate : gCandidates)
    {
        if (candidate.quality != quality)
            continue;
        if (candidate.requiredLevel < bracket.minLevel || candidate.requiredLevel > bracket.maxLevel)
            continue;
        if (candidate.itemLevel > maxItemLevel)
            continue;
        if (!anyEquipment && (!selection || !MatchesSelection(candidate, *selection)))
            continue;
        entries.push_back(candidate.entry);
    }
    return entries;
}

bool IsCandidateQualityReachable(uint32 quality)
{
    if (quality == ITEM_QUALITY_POOR)
        return gConfig.grayChance > 0;
    if (quality == ITEM_QUALITY_NORMAL)
        return gConfig.whiteChance > 0;

    // Green, blue, and epic intentionally fall back among one another when an
    // exact quality pool is empty, so any normal-equipment quality is reachable
    // whenever at least one of their weights is enabled.
    if (quality >= ITEM_QUALITY_UNCOMMON && quality <= ITEM_QUALITY_EPIC)
        return gConfig.greenChance > 0 || gConfig.blueChance > 0 || gConfig.epicChance > 0;

    return false;
}

bool HasRewardForSelection(PlayerState const& selection, uint32 bracketIndex)
{
    if (bracketIndex >= Brackets.size())
        return false;

    LevelBracket const& bracket = Brackets[bracketIndex];
    uint32 maxItemLevel = gConfig.maxItemLevels[bracketIndex];

    std::lock_guard<std::mutex> lock(gPoolMutex);
    for (Candidate const& candidate : gCandidates)
    {
        if (!IsCandidateQualityReachable(candidate.quality))
            continue;
        if (candidate.requiredLevel < bracket.minLevel || candidate.requiredLevel > bracket.maxLevel)
            continue;
        if (candidate.itemLevel > maxItemLevel)
            continue;
        if (MatchesSelection(candidate, selection))
            return true;
    }

    return false;
}

bool IsBracketAvailableToPlayer(Player const* player, uint32 bracketIndex)
{
    if (!player || bracketIndex >= Brackets.size())
        return false;

    LevelBracket const& bracket = Brackets[bracketIndex];
    if (bracket.requiredExpansion > gConfig.expansion)
        return false;
    if (gConfig.requirePlayerLevel && player->GetLevel() < bracket.minLevel)
        return false;
    return true;
}

bool HasAnyRewardForSelection(Player const* player, PlayerState const& selection)
{
    for (uint32 i = 0; i < Brackets.size(); ++i)
        if (IsBracketAvailableToPlayer(player, i) && HasRewardForSelection(selection, i))
            return true;
    return false;
}

uint32 SelectConsolationReward(PlayerState const* selection, uint32 rolledQuality, bool anyEquipment)
{
    if (rolledQuality == ITEM_QUALITY_POOR)
    {
        if (anyEquipment || !selection)
        {
            std::array<uint32, 3> rewards = {FALLBACK_GRAY_BUTTON, FALLBACK_GRAY_BOAR_TUSK, FALLBACK_GRAY_WAND};
            return rewards[urand(0, static_cast<uint32>(rewards.size() - 1))];
        }

        if (selection->kind == SelectionKind::Weapon)
        {
            std::array<uint32, 3> rewards = {FALLBACK_GRAY_WAND, FALLBACK_GRAY_BOAR_TUSK, FALLBACK_GRAY_BUTTON};
            return rewards[urand(0, static_cast<uint32>(rewards.size() - 1))];
        }

        std::array<uint32, 2> rewards = {FALLBACK_GRAY_BUTTON, FALLBACK_GRAY_BOAR_TUSK};
        return rewards[urand(0, static_cast<uint32>(rewards.size() - 1))];
    }

    if (rolledQuality == ITEM_QUALITY_NORMAL)
    {
        if (selection && selection->kind == SelectionKind::Armor && selection->subClass == ARMOR_LEATHER)
            return FALLBACK_WHITE_LEATHER_SCRAPS;

        if (anyEquipment || !selection || (selection && selection->kind == SelectionKind::Weapon))
            return urand(0, 1) ? FALLBACK_WHITE_ARMOR_SCRAPS : FALLBACK_WHITE_LEATHER_SCRAPS;

        return FALLBACK_WHITE_ARMOR_SCRAPS;
    }

    return 0;
}

uint32 SelectRewardEntry(PlayerState const* selection, uint32 bracketIndex, uint32 rolledQuality, bool anyEquipment)
{
    // Always try the exact rolled quality first.
    std::vector<uint32> entries = FindRewardEntries(selection, bracketIndex, rolledQuality, anyEquipment);
    if (!entries.empty())
        return entries[urand(0, static_cast<uint32>(entries.size() - 1))];

    // Gray and white rolls are intentional losing outcomes. If no exact-slot
    // equipment exists in the bracket, award the configured thematic junk
    // fallback instead of rerolling upward into a better quality.
    if (rolledQuality == ITEM_QUALITY_POOR || rolledQuality == ITEM_QUALITY_NORMAL)
        return SelectConsolationReward(selection, rolledQuality, anyEquipment);

    // Preserve the original behavior for green/blue/epic rolls: if the exact
    // quality has no candidate, fall back among the normal equipment qualities.
    std::array<uint32, 2> fallback{};
    if (rolledQuality == ITEM_QUALITY_EPIC)
        fallback = {ITEM_QUALITY_RARE, ITEM_QUALITY_UNCOMMON};
    else if (rolledQuality == ITEM_QUALITY_RARE)
        fallback = {ITEM_QUALITY_UNCOMMON, ITEM_QUALITY_EPIC};
    else
        fallback = {ITEM_QUALITY_RARE, ITEM_QUALITY_EPIC};

    for (uint32 quality : fallback)
    {
        entries = FindRewardEntries(selection, bracketIndex, quality, anyEquipment);
        if (!entries.empty())
            return entries[urand(0, static_cast<uint32>(entries.size() - 1))];
    }
    return 0;
}

bool StoreReward(Player* player, uint32 entry, uint32 count, int32 randomPropertyId = 0)
{
    ItemPosCountVec dest;
    InventoryResult result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count);
    if (result != EQUIP_ERR_OK)
    {
        player->SendEquipError(result, nullptr, nullptr, entry);
        return false;
    }

    Item* item = player->StoreNewItem(dest, entry, true, randomPropertyId);
    if (!item)
        return false;

    player->SendNewItem(item, count, true, false);
    return true;
}

bool GrantGamblingReward(Player* player, PlayerState const& state, uint32 bracketIndex)
{
    if (bracketIndex >= Brackets.size())
        return false;

    EnsurePoolLoaded();
    LevelBracket const& bracket = Brackets[bracketIndex];

    if (bracket.requiredExpansion > gConfig.expansion)
    {
        SendMessage(player, "That level range is sealed by the current expansion setting.");
        return false;
    }

    if (gConfig.requirePlayerLevel && player->GetLevel() < bracket.minLevel)
    {
        SendMessage(player, "You have not reached the minimum level for that wager.");
        return false;
    }

    uint32 price = GetConfiguredPriceGold(bracketIndex) * COPPER_PER_GOLD;
    if (player->GetMoney() < price)
    {
        std::ostringstream message;
        message << "You need " << GetConfiguredPriceGold(bracketIndex) << " gold for that wager.";
        SendMessage(player, message.str());
        return false;
    }

    uint32 rolledQuality = RollQuality();
    uint32 entry = SelectRewardEntry(&state, bracketIndex, rolledQuality, false);
    if (!entry)
    {
        SendMessage(player, "No valid item exists for that exact category and level range. No gold was taken.");
        return false;
    }

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    if (!proto)
        return false;

    ItemPosCountVec dest;
    InventoryResult canStore = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, 1);
    if (canStore != EQUIP_ERR_OK)
    {
        player->SendEquipError(canStore, nullptr, nullptr, entry);
        return false;
    }

    player->ModifyMoney(-static_cast<int32>(price));
    int32 randomPropertyId = proto->Quality == ITEM_QUALITY_UNCOMMON ? Item::GenerateItemRandomPropertyId(entry) : 0;
    Item* item = player->StoreNewItem(dest, entry, true, randomPropertyId);
    if (!item)
    {
        player->ModifyMoney(static_cast<int32>(price));
        SendMessage(player, "The transaction failed, so your gold was returned.");
        return false;
    }

    player->SendNewItem(item, 1, true, false);
    SendMessage(player, "The mists part, revealing your reward.");

    if (gConfig.logTransactions)
        LOG_INFO("module", "MysticMerchant: player {} spent {} copper and received item {} (bracket {}, quality {}).",
            player->GetGUID().GetCounter(), price, entry, bracketIndex, proto->Quality);
    return true;
}

bool OpenNativeContainer(Player* player, uint32 chestEntry)
{
    if (!CountItem(player, chestEntry))
        return false;

    ItemTemplate const* box = sObjectMgr->GetItemTemplate(chestEntry);
    if (!box)
        return false;

    bool const hasItemLoot = LootTemplates_Item.HaveLootFor(chestEntry);
    bool const hasMoneyLoot = box->MaxMoneyLoot > 0;
    if (!hasItemLoot && !hasMoneyLoot)
    {
        SendMessage(player, "That container has no native loot configured.");
        return false;
    }

    Loot loot;
    if (hasItemLoot && !loot.FillLoot(chestEntry, LootTemplates_Item, player, true, true))
    {
        SendMessage(player, "That container's native loot could not be generated.");
        return false;
    }

    if (hasMoneyLoot)
        loot.generateMoneyLoot(box->MinMoneyLoot, box->MaxMoneyLoot);

    std::vector<LootItem const*> eligibleItems;
    eligibleItems.reserve(loot.items.size() + loot.quest_items.size());
    auto collectEligible = [&](std::vector<LootItem> const& items)
    {
        for (LootItem const& lootItem : items)
            if (lootItem.AllowedForPlayer(player, ObjectGuid::Empty))
                eligibleItems.push_back(&lootItem);
    };
    collectEligible(loot.items);
    collectEligible(loot.quest_items);

    uint32 const requiredFreeSlots = std::max<uint32>(
        gConfig.chestMinimumFreeSlots, static_cast<uint32>(eligibleItems.size()));
    if (CountFreeGeneralInventorySlots(player) < requiredFreeSlots)
    {
        std::ostringstream message;
        message << "Please keep at least " << requiredFreeSlots
                << " empty general bag slots before opening this container.";
        SendMessage(player, message.str());
        return false;
    }

    // Check unique limits and other item-specific restrictions before consuming the box.
    for (LootItem const* lootItem : eligibleItems)
    {
        ItemPosCountVec dest;
        InventoryResult result = player->CanStoreNewItem(
            NULL_BAG, NULL_SLOT, dest, lootItem->itemid, lootItem->count);
        if (result != EQUIP_ERR_OK)
        {
            player->SendEquipError(result, nullptr, nullptr, lootItem->itemid);
            SendMessage(player, "The container was not consumed because one of its rolled items cannot be stored.");
            return false;
        }
    }

    // Consume first so a failed later store cannot duplicate the box.
    player->DestroyItemCount(chestEntry, 1, true);

    uint32 granted = 0;
    uint32 mailed = 0;
    for (LootItem const* lootItem : eligibleItems)
    {
        if (StoreReward(player, lootItem->itemid, lootItem->count, lootItem->randomPropertyId))
        {
            ++granted;
            continue;
        }

        // Preserve the rolled random property when recovery mail is needed.
        if (Item* recoveryItem = Item::CreateItem(lootItem->itemid, lootItem->count, player, false, lootItem->randomPropertyId))
            player->SendItemRetrievalMail(recoveryItem);
        else
            player->SendItemRetrievalMail(lootItem->itemid, lootItem->count);
        ++mailed;
    }

    if (loot.gold)
        player->ModifyMoney(static_cast<int32>(loot.gold));

    if (granted == 0 && mailed == 0 && loot.gold == 0)
        SendMessage(player, "The container was empty.");
    else if (mailed)
        SendMessage(player, "The lock dissolves. Most contents appear in your bags; an unstored reward was sent by recovery mail.");
    else
        SendMessage(player, "The lock dissolves and the container's contents appear in your bags.");

    if (gConfig.logTransactions)
        LOG_INFO("module", "MysticMerchant: player {} opened native container {} and received {} item stacks, {} mailed stacks, and {} copper.",
            player->GetGUID().GetCounter(), chestEntry, granted, mailed, loot.gold);
    return true;
}

bool OpenReplacementContainer(Player* player, uint32 chestEntry)
{
    if (!CountItem(player, chestEntry))
        return false;

    EnsurePoolLoaded();
    ItemTemplate const* box = sObjectMgr->GetItemTemplate(chestEntry);
    uint32 estimatedLevel = EstimateContainerLevel(box);
    int32 bracketIndex = FindBracketForLevel(estimatedLevel);
    if (bracketIndex < 0)
        bracketIndex = estimatedLevel < 10 ? 0 : static_cast<int32>(Brackets.size() - 1);

    while (bracketIndex > 0 && Brackets[bracketIndex].requiredExpansion > gConfig.expansion)
        --bracketIndex;

    uint32 rolledQuality = RollQuality();
    uint32 rewardEntry = SelectRewardEntry(nullptr, static_cast<uint32>(bracketIndex), rolledQuality, true);
    if (!rewardEntry)
    {
        SendMessage(player, "No equivalent equipment reward could be generated. The box was not consumed.");
        return false;
    }

    ItemTemplate const* reward = sObjectMgr->GetItemTemplate(rewardEntry);
    ItemPosCountVec dest;
    InventoryResult canStore = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, rewardEntry, 1);
    if (canStore != EQUIP_ERR_OK)
    {
        player->SendEquipError(canStore, nullptr, nullptr, rewardEntry);
        return false;
    }

    player->DestroyItemCount(chestEntry, 1, true);
    int32 randomPropertyId = reward && reward->Quality == ITEM_QUALITY_UNCOMMON ? Item::GenerateItemRandomPropertyId(rewardEntry) : 0;
    Item* item = player->StoreNewItem(dest, rewardEntry, true, randomPropertyId);
    if (!item)
    {
        // Very unlikely after CanStoreNewItem; restore the box if creation fails.
        StoreReward(player, chestEntry, 1);
        SendMessage(player, "The exchange failed, so the container was restored.");
        return false;
    }

    player->SendNewItem(item, 1, true, false);
    SendMessage(player, "The sealed box fades away, leaving an equipment reward of comparable level.");

    if (gConfig.logTransactions)
        LOG_INFO("module", "MysticMerchant: player {} exchanged container {} for item {} (bracket {}).",
            player->GetGUID().GetCounter(), chestEntry, rewardEntry, bracketIndex);
    return true;
}

bool OpenOneContainer(Player* player, uint32 chestEntry)
{
    return gConfig.chestMode == 0 ? OpenNativeContainer(player, chestEntry) : OpenReplacementContainer(player, chestEntry);
}

std::string ItemName(uint32 entry)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
    return proto ? proto->Name1 : ("Item " + std::to_string(entry));
}

void ShowMainMenu(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    PlayerState& state = GetState(player);
    state = PlayerState{};

    if (gConfig.chestEnabled)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Open a locked container", GOSSIP_SENDER_MAIN, ACTION_OPEN_CHESTS);
    if (gConfig.gamblingEnabled)
    {
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Gamble for armor", GOSSIP_SENDER_MAIN, ACTION_GAMBLE_ARMOR);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Gamble for accessories", GOSSIP_SENDER_MAIN, ACTION_GAMBLE_ACCESSORIES);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Gamble for shields and off-hands", GOSSIP_SENDER_MAIN, ACTION_GAMBLE_OFFHANDS);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Gamble for a weapon", GOSSIP_SENDER_MAIN, ACTION_GAMBLE_WEAPONS);
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Explain your services", GOSSIP_SENDER_MAIN, ACTION_EXPLAIN);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Goodbye", GOSSIP_SENDER_MAIN, ACTION_CLOSE);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowExplanation(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT,
        "I can dissolve locks and reproduce a container's normal loot. I can also exchange gold for a random uncommon, rare, or epic equipment item from the configured loot-source equipment pool.",
        GOSSIP_SENDER_MAIN, ACTION_EXPLAIN);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT,
        "Uncommon rewards receive their normal random property or suffix when the item supports one. Expansion and item-level limits are controlled by the module config.",
        GOSSIP_SENDER_MAIN, ACTION_EXPLAIN);
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowChestList(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    PlayerState& state = GetState(player);
    state.selectedChest = 0;
    state.chestEntries.clear();

    std::map<uint32, uint32> boxes = FindLockedContainers(player);
    uint32 index = 0;
    for (auto const& [entry, count] : boxes)
    {
        if (index >= 30)
            break;
        std::ostringstream label;
        label << ItemName(entry) << " x" << count;
        state.chestEntries.push_back(entry);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, label.str(), GOSSIP_SENDER_MAIN, ACTION_CHEST_ENTRY_BASE + index);
        ++index;
    }

    if (state.chestEntries.empty())
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No supported locked containers were found in your bags.", GOSSIP_SENDER_MAIN, ACTION_OPEN_CHESTS);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowChestActions(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    PlayerState& state = GetState(player);
    uint32 count = CountItem(player, state.selectedChest);
    if (!state.selectedChest || count == 0)
    {
        state.selectedChest = 0;
        ShowChestList(player, creature);
        return;
    }

    std::ostringstream title;
    title << ItemName(state.selectedChest) << " x" << count;
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, title.str(), GOSSIP_SENDER_MAIN, ACTION_CHEST_BACK);

    if (gConfig.chestOpenOne)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Open one", GOSSIP_SENDER_MAIN, ACTION_CHEST_OPEN_ONE,
            "Open one of these containers?", 0, false);
    if (gConfig.chestOpenAll)
    {
        std::ostringstream label;
        label << "Open all of this type (maximum " << gConfig.chestOpenAllMax << ")";
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, label.str(), GOSSIP_SENDER_MAIN, ACTION_CHEST_OPEN_ALL,
            "Open every container of this type up to the configured batch maximum?", 0, false);
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_CHEST_BACK);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowArmorTypes(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    std::array<std::pair<uint32, char const*>, 4> const armorTypes = {{
        {ARMOR_CLOTH, "Cloth"}, {ARMOR_LEATHER, "Leather"},
        {ARMOR_MAIL, "Mail"}, {ARMOR_PLATE, "Plate"}
    }};
    std::array<SlotChoice, 8> const armorSlots = {{
        SlotChoice::Head, SlotChoice::Shoulders, SlotChoice::Chest, SlotChoice::Wrists,
        SlotChoice::Hands, SlotChoice::Waist, SlotChoice::Legs, SlotChoice::Feet
    }};

    for (auto const& [subClass, name] : armorTypes)
    {
        bool available = false;
        for (SlotChoice slot : armorSlots)
        {
            PlayerState selection;
            selection.kind = SelectionKind::Armor;
            selection.subClass = subClass;
            selection.slot = slot;
            if (HasAnyRewardForSelection(player, selection))
            {
                available = true;
                break;
            }
        }
        if (available)
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name, GOSSIP_SENDER_MAIN, ACTION_ARMOR_BASE + subClass);
    }

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowArmorSlots(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    PlayerState const& state = GetState(player);
    std::array<std::pair<SlotChoice, char const*>, 8> const slots = {{
        {SlotChoice::Head, "Head"}, {SlotChoice::Shoulders, "Shoulders"}, {SlotChoice::Chest, "Chest"},
        {SlotChoice::Wrists, "Wrists"}, {SlotChoice::Hands, "Hands"}, {SlotChoice::Waist, "Waist"},
        {SlotChoice::Legs, "Legs"}, {SlotChoice::Feet, "Feet"}
    }};
    for (auto const& [slot, name] : slots)
    {
        PlayerState selection = state;
        selection.slot = slot;
        if (HasAnyRewardForSelection(player, selection))
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name, GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + static_cast<uint32>(slot));
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_GAMBLE_ARMOR);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowAccessorySlots(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    std::array<std::pair<SlotChoice, char const*>, 4> const slots = {{
        {SlotChoice::Back, "Back / Cloak"}, {SlotChoice::Neck, "Neck"},
        {SlotChoice::Finger, "Finger"}, {SlotChoice::Trinket, "Trinket"}
    }};
    for (auto const& [slot, name] : slots)
    {
        PlayerState selection;
        selection.kind = SelectionKind::Accessory;
        selection.slot = slot;
        if (HasAnyRewardForSelection(player, selection))
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name, GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + static_cast<uint32>(slot));
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowOffHandSlots(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    std::array<std::pair<SlotChoice, char const*>, 3> const slots = {{
        {SlotChoice::Shield, "Shield"}, {SlotChoice::HeldInOffHand, "Held in off-hand"},
        {SlotChoice::Relic, "Relic (libram, idol, totem, sigil)"}
    }};
    for (auto const& [slot, name] : slots)
    {
        PlayerState selection;
        selection.kind = SelectionKind::OffHand;
        selection.slot = slot;
        if (HasAnyRewardForSelection(player, selection))
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name, GOSSIP_SENDER_MAIN, ACTION_SLOT_BASE + static_cast<uint32>(slot));
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowWeaponTypes(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    std::array<std::pair<uint32, char const*>, 16> const weapons = {{
        {WEAPON_AXE_1H, "One-handed axe"}, {WEAPON_AXE_2H, "Two-handed axe"},
        {WEAPON_MACE_1H, "One-handed mace"}, {WEAPON_MACE_2H, "Two-handed mace"},
        {WEAPON_SWORD_1H, "One-handed sword"}, {WEAPON_SWORD_2H, "Two-handed sword"},
        {WEAPON_DAGGER, "Dagger"}, {WEAPON_FIST, "Fist weapon"}, {WEAPON_POLEARM, "Polearm"},
        {WEAPON_STAFF, "Staff"}, {WEAPON_BOW, "Bow"}, {WEAPON_CROSSBOW, "Crossbow"},
        {WEAPON_GUN, "Gun"}, {WEAPON_WAND, "Wand"}, {WEAPON_THROWN, "Thrown weapon"},
        {WEAPON_FISHING_POLE, "Fishing pole"}
    }};
    for (auto const& [subClass, name] : weapons)
    {
        PlayerState selection;
        selection.kind = SelectionKind::Weapon;
        selection.subClass = subClass;
        if (HasAnyRewardForSelection(player, selection))
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, name, GOSSIP_SENDER_MAIN, ACTION_WEAPON_BASE + subClass);
    }
    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

void ShowLevelRanges(Player* player, Creature* creature)
{
    ClearGossipMenuFor(player);
    PlayerState& state = GetState(player);
    bool showedRange = false;

    for (uint32 i = 0; i < Brackets.size(); ++i)
    {
        LevelBracket const& bracket = Brackets[i];
        if (!IsBracketAvailableToPlayer(player, i))
            continue;
        if (!HasRewardForSelection(state, i))
            continue;

        showedRange = true;
        std::ostringstream label;
        label << bracket.label << " - " << GetConfiguredPriceGold(i) << " gold";
        std::ostringstream confirm;
        confirm << "Spend " << GetConfiguredPriceGold(i) << " gold for one random item?";
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, label.str(), GOSSIP_SENDER_MAIN, ACTION_LEVEL_BASE + i,
            confirm.str(), 0, false);
    }

    if (!showedRange)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "No eligible rewards exist for this selection at your available levels.", GOSSIP_SENDER_MAIN, ACTION_MAIN);

    AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back to main menu", GOSSIP_SENDER_MAIN, ACTION_MAIN);
    SendGossipMenuFor(player, GOSSIP_TEXT_ID, creature->GetGUID());
}

class MysticMerchantCreatureScript : public CreatureScript
{
public:
    MysticMerchantCreatureScript() : CreatureScript("npc_mystic_merchant") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (creature->GetEntry() != gConfig.npcEntry)
            return false;

        if (!gConfig.enabled)
        {
            SendMessage(player, "The merchant's magic is currently disabled.");
            CloseGossipMenuFor(player);
            return true;
        }

        EnsurePoolLoaded();
        ShowMainMenu(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (creature->GetEntry() != gConfig.npcEntry)
            return false;

        if (sender != GOSSIP_SENDER_MAIN)
            return false;

        if (!gConfig.enabled)
        {
            SendMessage(player, "The merchant's magic is currently disabled.");
            CloseGossipMenuFor(player);
            return true;
        }

        bool const chestAction = action == ACTION_OPEN_CHESTS || action == ACTION_CHEST_OPEN_ONE ||
            action == ACTION_CHEST_OPEN_ALL || action == ACTION_CHEST_BACK ||
            (action >= ACTION_CHEST_ENTRY_BASE && action < ACTION_CHEST_ENTRY_BASE + 100);
        if (chestAction && !gConfig.chestEnabled)
        {
            ShowMainMenu(player, creature);
            return true;
        }

        bool const gamblingAction = action == ACTION_GAMBLE_ARMOR || action == ACTION_GAMBLE_ACCESSORIES ||
            action == ACTION_GAMBLE_OFFHANDS || action == ACTION_GAMBLE_WEAPONS ||
            (action >= ACTION_ARMOR_BASE + ARMOR_CLOTH && action <= ACTION_ARMOR_BASE + ARMOR_PLATE) ||
            (action >= ACTION_SLOT_BASE + 1 && action < ACTION_SLOT_BASE + 100) ||
            (action >= ACTION_WEAPON_BASE && action < ACTION_WEAPON_BASE + 100) ||
            (action >= ACTION_LEVEL_BASE && action < ACTION_LEVEL_BASE + Brackets.size());
        if (gamblingAction && !gConfig.gamblingEnabled)
        {
            ShowMainMenu(player, creature);
            return true;
        }

        ClearGossipMenuFor(player);
        PlayerState& state = GetState(player);

        if (action == ACTION_MAIN)
            ShowMainMenu(player, creature);
        else if (action == ACTION_CLOSE)
            CloseGossipMenuFor(player);
        else if (action == ACTION_EXPLAIN)
            ShowExplanation(player, creature);
        else if (action == ACTION_OPEN_CHESTS)
            ShowChestList(player, creature);
        else if (action == ACTION_GAMBLE_ARMOR)
        {
            state.kind = SelectionKind::Armor;
            ShowArmorTypes(player, creature);
        }
        else if (action == ACTION_GAMBLE_ACCESSORIES)
        {
            state.kind = SelectionKind::Accessory;
            ShowAccessorySlots(player, creature);
        }
        else if (action == ACTION_GAMBLE_OFFHANDS)
        {
            state.kind = SelectionKind::OffHand;
            ShowOffHandSlots(player, creature);
        }
        else if (action == ACTION_GAMBLE_WEAPONS)
        {
            state.kind = SelectionKind::Weapon;
            ShowWeaponTypes(player, creature);
        }
        else if (action >= ACTION_ARMOR_BASE + ARMOR_CLOTH && action <= ACTION_ARMOR_BASE + ARMOR_PLATE)
        {
            state.kind = SelectionKind::Armor;
            state.subClass = action - ACTION_ARMOR_BASE;
            ShowArmorSlots(player, creature);
        }
        else if (action >= ACTION_SLOT_BASE + 1 && action < ACTION_SLOT_BASE + 100)
        {
            state.slot = static_cast<SlotChoice>(action - ACTION_SLOT_BASE);
            ShowLevelRanges(player, creature);
        }
        else if (action >= ACTION_WEAPON_BASE && action < ACTION_WEAPON_BASE + 100)
        {
            state.kind = SelectionKind::Weapon;
            state.subClass = action - ACTION_WEAPON_BASE;
            state.slot = SlotChoice::None;
            ShowLevelRanges(player, creature);
        }
        else if (action >= ACTION_LEVEL_BASE && action < ACTION_LEVEL_BASE + Brackets.size())
        {
            uint32 bracketIndex = action - ACTION_LEVEL_BASE;
            GrantGamblingReward(player, state, bracketIndex);
            ShowLevelRanges(player, creature);
        }
        else if (action >= ACTION_CHEST_ENTRY_BASE && action < ACTION_CHEST_ENTRY_BASE + 100)
        {
            uint32 index = action - ACTION_CHEST_ENTRY_BASE;
            if (index < state.chestEntries.size())
            {
                state.selectedChest = state.chestEntries[index];
                ShowChestActions(player, creature);
            }
            else
                ShowChestList(player, creature);
        }
        else if (action == ACTION_CHEST_OPEN_ONE)
        {
            if (state.selectedChest)
                OpenOneContainer(player, state.selectedChest);
            ShowChestActions(player, creature);
        }
        else if (action == ACTION_CHEST_OPEN_ALL)
        {
            uint32 opened = 0;
            uint32 target = std::min<uint32>(CountItem(player, state.selectedChest), gConfig.chestOpenAllMax);
            while (opened < target && CountItem(player, state.selectedChest) > 0)
            {
                if (!OpenOneContainer(player, state.selectedChest))
                    break;
                ++opened;
            }
            std::ostringstream message;
            message << "Opened " << opened << " container" << (opened == 1 ? "" : "s") << ".";
            SendMessage(player, message.str());
            ShowChestActions(player, creature);
        }
        else if (action == ACTION_CHEST_BACK)
            ShowChestList(player, creature);
        else
            ShowMainMenu(player, creature);

        return true;
    }
};

class MysticMerchantWorldScript : public WorldScript
{
public:
    MysticMerchantWorldScript() : WorldScript("MysticMerchantWorldScript", {WORLDHOOK_ON_BEFORE_CONFIG_LOAD, WORLDHOOK_ON_STARTUP}) { }

    void OnBeforeConfigLoad(bool reload) override
    {
        LoadConfig();
        if (reload)
        {
            gPoolLoaded.store(false);
            LoadRewardPool();
        }
    }

    void OnStartup() override
    {
        LoadRewardPool();
        LOG_INFO("module", "MysticMerchant: enabled={}, NPC={}, expansion={}, chest mode={}, quality weights gray/white/green/blue/epic={}/{}/{}/{}/{}.",
            gConfig.enabled, gConfig.npcEntry, gConfig.expansion, gConfig.chestMode,
            gConfig.grayChance, gConfig.whiteChance, gConfig.greenChance, gConfig.blueChance, gConfig.epicChance);
    }
};

class MysticMerchantPlayerScript : public PlayerScript
{
public:
    MysticMerchantPlayerScript() : PlayerScript("MysticMerchantPlayerScript", {PLAYERHOOK_ON_LOGOUT}) { }

    void OnPlayerLogout(Player* player) override
    {
        ClearState(player);
    }
};
} // namespace MysticMerchant

void Addmod_mystic_merchantScripts()
{
    new MysticMerchant::MysticMerchantCreatureScript();
    new MysticMerchant::MysticMerchantWorldScript();
    new MysticMerchant::MysticMerchantPlayerScript();
}
