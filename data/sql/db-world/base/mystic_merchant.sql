-- Mystic Merchant NPC
-- Entry 900201 is intentionally outside Trinity's custom subclass trainer range.
-- Schema target: current AzerothCore creature_template / creature_template_model.

DELETE FROM `creature_template_model` WHERE `CreatureID` = 900201;
DELETE FROM `creature_template` WHERE `entry` = 900201;

INSERT INTO `creature_template`
(`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
 `unit_class`, `unit_flags`, `type`, `AIName`, `MovementType`, `RegenHealth`, `flags_extra`, `ScriptName`, `VerifiedBuild`)
VALUES
(900201, 'Mystic Merchant', 'Keeper of Sealed Fortunes', 'Speak', 0, 80, 80, 2, 35, 1,
 1, 2, 7, '', 0, 1, 16785408, 'npc_mystic_merchant', 0);

-- flags_extra combines CANNOT_ENTER_COMBAT (8192) and MODULE (16777216).
-- Default display. Replace CreatureDisplayID if you prefer another merchant model.
INSERT INTO `creature_template_model`
(`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES (900201, 0, 2299, 1.0, 1.0, 0);
