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

export const CLASS_COLOR: Record<number, string> = {
  1: '#c69b6d', 2: '#f48cba', 3: '#aad372', 4: '#fff468', 5: '#ffffff',
  6: '#c41e3a', 7: '#0070dd', 8: '#3fc7eb', 9: '#8788ee', 11: '#ff7c0a',
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
}

export const zoneName = (id: number) => ZONES[id] ?? `Zone ${id}`
