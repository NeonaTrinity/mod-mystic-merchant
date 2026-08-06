# mod-mystic-merchant

Standalone AzerothCore 3.3.5a module providing a scripted Mystic Merchant NPC.

## Features

- Creates NPC entry `900201`, spawnable with `.npc add 900201`.
- Scans the player's backpack and equipped bags for supported locked containers.
- Lets the player open one container or all containers of one selected type, up to a configurable batch maximum.
- Native chest mode rolls the selected box's normal item loot and money loot, including loot conditions and normal random properties.
- Replacement chest mode consumes a box and grants one random equipment item from a comparable level bracket.
- Gambling paths for:
  - Cloth, leather, mail, and plate armor.
  - Head, shoulders, chest/robe, wrists, hands, waist, legs, and feet.
  - Cloaks, necks, rings, and trinkets.
  - Shields, held-in-off-hand items, and relics.
  - Every WotLK weapon subclass, including fishing poles.
- Material, slot, weapon, and level choices are hidden when no valid reward exists for that selection.
- Configurable Vanilla, Burning Crusade, or Wrath level cap.
- Configurable green, blue, and epic odds and all requested bracket prices.
- Green rewards use AzerothCore's normal random-property/suffix generator.
- Reward pool defaults to equippable items referenced by creature, game-object, or reference loot tables.
- Conservative configurable item-level ceilings limit higher-tier equipment.
- Transaction logging for wagers and container exchanges.

## Included files

```text
mod-mystic-merchant/
├── conf/
│   └── mod_mystic_merchant.conf.dist
├── data/sql/db-world/base/
│   └── mystic_merchant.sql
├── src/
│   └── mod_mystic_merchant.cpp
├── include.sh
├── LICENSE.md
└── README.md
```

## Installation

1. Extract the complete `mod-mystic-merchant` folder into the AzerothCore project's `modules/` directory.
2. Reconfigure and rebuild the server normally. For the standard Docker setup, rebuilding the compose services is sufficient.
3. Allow AzerothCore's database assembler to import the module SQL, or manually import:

   ```text
   modules/mod-mystic-merchant/data/sql/db-world/base/mystic_merchant.sql
   ```

   into `acore_world`.
4. Copy the installed `mod_mystic_merchant.conf.dist` file to `mod_mystic_merchant.conf` in the runtime modules config directory if your deployment does not create that copy automatically.
5. Restart the worldserver.
6. Spawn the merchant in game:

   ```text
   .npc add 900201
   ```

The `MysticMerchant.NpcEntry` setting must match the entry created by the SQL. When changing it, update both the config and SQL before rebuilding/importing.

For servers that validate custom creature IDs, add `900201` to `Creatures.CustomIDs` in `worldserver.conf` if the startup log requests it.

## Docker example

From the AzerothCore project directory after placing this module under `modules/`:

```bash
docker compose up -d --build
```

Manual SQL import example for the common AzerothCore Docker service names:                                                                                                      

```bash
docker compose exec -T ac-database mysql -uroot -ppassword acore_world \
  < modules/mod-mystic-merchant/data/sql/db-world/base/mystic_merchant.sql
```

Use the actual database password if it differs.

--------- 
copy the .conf.dist file and rename the copy to .conf.

From inside ~/wow-server-playerbots, run:

cp modules/mod-mystic-merchant/conf/mod_mystic_merchant.conf.dist \
   env/dist/etc/modules/mod_mystic_merchant.conf

## Main gossip menu

```text
Open a locked container
Gamble for armor
Gamble for accessories
Gamble for shields and off-hands
Gamble for a weapon
Explain your services
Goodbye
```

The module rechecks the selected item, available money, bag space, player level, and reward pool when the final action is clicked. A stale or invalid gossip selection does not consume gold or a container.

## Expansion behavior

- `MysticMerchant.Expansion = 0`: shows level 10-60 brackets only.
- `MysticMerchant.Expansion = 1`: shows level 10-70 brackets only.
- `MysticMerchant.Expansion = 2`: shows all brackets through level 80.

The expansion setting hides later choices and blocks replacement-container rewards from selecting those brackets.

## Gambling prices

```text
Level 10-19      2 gold
Level 20-29      5 gold
Level 30-39      10 gold
Level 40-49      20 gold
Level 50-59      30 gold
Level 60        50 gold
Level 61-69     75 gold
Level 70        100 gold
Level 71-79     150 gold
Level 80        250 gold

can be modified with these config settings:

MysticMerchant.Gambling.Price.10_19 = 2
MysticMerchant.Gambling.Price.20_29 = 5
MysticMerchant.Gambling.Price.30_39 = 10
MysticMerchant.Gambling.Price.40_49 = 20
MysticMerchant.Gambling.Price.50_59 = 30
MysticMerchant.Gambling.Price.60 = 50
MysticMerchant.Gambling.Price.61_69 = 75
MysticMerchant.Gambling.Price.70 = 100
MysticMerchant.Gambling.Price.71_79 = 150
MysticMerchant.Gambling.Price.80 = 250

```

All prices and quality odds are configurable. With `MysticMerchant.Gambling.RequirePlayerLevel = 1`, a player only sees brackets whose minimum level they have reached.

The default quality roll is:

```text
Uncommon (green): 80%
Rare (blue):      18%
Epic (purple):     2%
```

When the rolled quality has no valid item for an otherwise valid selection, the module falls back to another available configured quality rather than taking gold and returning nothing.

## Chest modes

### Mode 0: native loot

```ini
MysticMerchant.Chests.Mode = 0
```

The merchant rolls the selected box through its normal `item_loot_template`, evaluates the rolled loot for the player, and reproduces eligible item stacks and money loot.

Before consuming the box, the module:

- Confirms the box is still present.
- Generates the complete roll.
- Checks a configurable minimum number of empty general-purpose bag slots.
- Raises that required slot count when the roll contains more item stacks.
- Preflights each item through `CanStoreNewItem`.

The box is consumed only after those checks pass. If inventory state changes unexpectedly after that preflight, an unstored item stack is sent through AzerothCore's recovery-mail path.

### Mode 1: replacement equipment

```ini
MysticMerchant.Chests.Mode = 1
```

The module estimates the box level from `RequiredLevel`, then `ItemLevel`, maps it to an enabled bracket, rolls quality, and grants one random armor or weapon item from that bracket.

If no valid reward can be selected or stored, the box is not consumed. If item creation unexpectedly fails after consumption, the module attempts to restore the original box.

## Reward-pool behavior

With:

```ini
MysticMerchant.Pool.UseLootSourcesOnly = 1
```

the module loads direct item IDs referenced by:

- `creature_loot_template`
- `gameobject_loot_template`
- `reference_loot_template`

`item_loot_template` can also be included with `MysticMerchant.Pool.IncludeItemLoot = 1`.

The candidate list is then filtered to equippable uncommon, rare, or epic armor and weapons. Deprecated entries are always rejected. Item sets, reputation items, quest-related items, and conjured items are rejected by default but can be enabled separately.

Loot-table presence does not perfectly identify whether an item originated in a raid. The default item-level ceilings are intentionally conservative, but a same-item-level raid reward can still enter a broad source pool. Lower the ceiling values or curate the underlying loot tables when a strictly raid-free pool is required.

## Validation checklist

1. The worldserver log reports that Mystic Merchant loaded eligible equipment entries.
2. `.npc add 900201` creates the merchant and opens the main gossip menu.
3. A supported locked box appears by item name and quantity.
4. Native mode consumes exactly one selected box and reproduces its generated items and money.
5. `Open all` stops at `MysticMerchant.Chests.OpenAllMaximum` and never consumes a failed box.
6. A level-35 character sees brackets through 30-39 when level gating is enabled.
7. A 30-39 wager deducts exactly 20 gold and grants the selected category.
8. A green template that supports random properties can receive a normal suffix such as `of the Bear`.
9. Expansion 0 hides level 61+ brackets; expansion 1 hides level 71+ brackets.
10. Empty reward combinations do not appear in material, slot, weapon, or level menus.

## Compatibility note

The module targets the current AzerothCore master-style module layout, script hooks, loot APIs, and world database schema available when it was written. Compile it against the exact server checkout before live deployment because custom forks can rename hooks, change enum values, or alter database columns.
