// Fixed continent bounds in world coordinates. Deliberately not derived from live
// data: bounds that move as bots wander make the map jump between refreshes.
// Exact bounds read from the client's own WorldMapArea.dbc, not estimated. In DBC
// terms LocLeft/LocRight are world Y and LocTop/LocBottom are world X, which is why
// screen X comes from Y below.
export const MAPS: Record<number, { name: string; left: number; right: number; top: number; bottom: number }> = {
  0:   { name: 'Eastern Kingdoms', left: 18172.0, right: -22569.2, top: 11176.3, bottom: -15973.3 },
  1:   { name: 'Kalimdor',         left: 17066.6, right: -19733.2, top: 12799.9, bottom: -11733.3 },
  530: { name: 'Outland',          left: 12996.0, right:  -4468.0, top:  5821.4, bottom:  -5821.4 },
  571: { name: 'Northrend',        left:  9217.2, right:  -8534.2, top: 10593.4, bottom:  -1240.9 },
}

export const CLASSES: Record<number, string> = {
  1: 'Warrior', 2: 'Paladin', 3: 'Hunter', 4: 'Rogue', 5: 'Priest',
  6: 'Death Knight', 7: 'Shaman', 8: 'Mage', 9: 'Warlock', 11: 'Druid',
}

// Blizzard's class colours, with three exceptions. Death Knight (#C41F3B), Shaman
// (#0070DE) and Warlock (#9482C9) were picked for a black chat window; on this warm
// dark plate they sit too close to the ground to read at 12px. Those three keep their
// hue and are lifted in lightness only, so a death knight is still unmistakably red.
export const CLASS_COLOR: Record<number, string> = {
  1: '#c79c6e',   // warrior
  2: '#f58cba',   // paladin
  3: '#abd473',   // hunter
  4: '#fff569',   // rogue
  5: '#ffffff',   // priest
  6: '#e04a62',   // death knight - lifted from #c41f3b
  7: '#3f96f0',   // shaman - lifted from #0070de
  8: '#69ccf0',   // mage
  9: '#b3a2e8',   // warlock - lifted from #9482c9
  11: '#ff7d0a',  // druid
}

// The DBC tables in this realm are empty, so zone names are curated. Anything not
// listed renders as its numeric id rather than guessing.
export const ZONES: Record<number, string> = {
  1: 'Dun Morogh', 3: 'Badlands', 4: 'Blasted Lands', 8: 'Swamp of Sorrows',
  10: 'Duskwood', 11: 'Wetlands', 12: 'Elwynn Forest', 14: 'Durotar',
  15: 'Dustwallow Marsh', 16: 'Azshara', 17: 'The Barrens', 28: 'Western Plaguelands',
  33: 'Stranglethorn Vale', 36: 'Alterac Mountains', 38: 'Loch Modan', 40: 'Westfall',
  41: 'Deadwind Pass', 44: 'Redridge Mountains', 45: 'Arathi Highlands',
  46: 'Burning Steppes', 47: 'The Hinterlands', 51: 'Searing Gorge',
  65: 'Dragonblight', 66: "Zul'Drak", 67: 'The Storm Peaks', 85: 'Tirisfal Glades',
  130: 'Silverpine Forest', 139: 'Eastern Plaguelands', 141: 'Teldrassil',
  148: 'Darkshore', 210: 'Icecrown', 215: 'Mulgore', 267: 'Hillsbrad Foothills',
  331: 'Ashenvale', 357: 'Feralas', 361: 'Felwood', 394: 'Grizzly Hills',
  400: 'Thousand Needles', 405: 'Desolace', 406: 'Stonetalon Mountains',
  440: 'Tanaris', 490: "Un'Goro Crater", 493: 'Moonglade', 495: 'Howling Fjord',
  618: 'Winterspring', 1377: 'Silithus', 1497: 'Undercity', 1519: 'Stormwind City',
  1537: 'Ironforge', 1637: 'Orgrimmar', 1638: 'Thunder Bluff', 1657: 'Darnassus',
  3430: 'Eversong Woods', 3433: 'Ghostlands', 3483: 'Hellfire Peninsula',
  3487: 'Silvermoon City', 3518: 'Nagrand', 3519: 'Terokkar Forest',
  3520: 'Shadowmoon Valley', 3521: 'Zangarmarsh', 3522: "Blade's Edge Mountains",
  3523: 'Netherstorm', 3524: 'Azuremyst Isle', 3525: 'Bloodmyst Isle',
  3537: 'Borean Tundra', 3557: 'The Exodar', 3703: 'Shattrath City',
  3711: 'Sholazar Basin', 4197: 'Wintergrasp', 4395: 'Dalaran',
  4742: "Hrothgar's Landing",
  // Instance and sub-zone ids that show up constantly once bots start running dungeons.
  25: 'Blackrock Mountain', 209: 'Shadowfang Keep', 491: 'Razorfen Kraul',
  717: 'The Stockade', 718: 'Wailing Caverns', 719: 'The Deadmines',
  721: 'Gnomeregan', 722: 'Razorfen Downs', 796: 'Scarlet Monastery',
  1337: 'Uldaman', 1477: "Temple of Atal'Hakkar", 1581: 'The Deadmines',
  1583: 'Blackrock Spire', 1584: 'Blackrock Depths', 2017: 'Stratholme',
  2057: 'Scholomance', 2100: 'Maraudon', 2557: 'Dire Maul',
}

export const zoneName = (id: number) => ZONES[id] ?? `Zone ${id}`
