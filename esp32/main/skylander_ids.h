#pragma once
#ifndef SKYLANDER_IDS_H
#define SKYLANDER_IDS_H

#include <stdint.h>

/*
 * Character ID → name lookup.
 * The character ID lives at bytes 0x11-0x12 (little-endian uint16) in the
 * decrypted dump. IDs from the Texthead1/Skylander-IDs community database.
 */

typedef struct {
    uint16_t    id;
    const char *name;
    const char *element;  /* Fire/Water/Earth/Air/Life/Undead/Magic/Tech/Light/Dark */
} skylander_info_t;

/* Returns name for a given character ID, or NULL if unknown */
static inline const char *skylander_name_from_id(uint16_t id) {
    /* clang-format off */
    static const skylander_info_t table[] = {
        /* Spyro's Adventure */
        {  0, "Whirlwind",       "Air"    },
        {  1, "Sonic Boom",      "Air"    },
        {  2, "Warnado",         "Air"    },
        {  3, "Lightning Rod",   "Air"    },
        {  4, "Bash",            "Earth"  },
        {  5, "Terrafin",        "Earth"  },
        {  6, "Dino-Rang",       "Earth"  },
        {  7, "Prism Break",     "Earth"  },
        {  8, "Flameslinger",    "Fire"   },
        {  9, "Eruptor",         "Fire"   },
        { 10, "Ignitor",         "Fire"   },
        { 11, "Sunburn",         "Fire"   },
        { 12, "Zap",             "Water"  },
        { 13, "Wham-Shell",      "Water"  },
        { 14, "Gill Grunt",      "Water"  },
        { 15, "Slam Bam",        "Water"  },
        { 16, "Spyro",           "Magic"  },
        { 17, "Voodood",         "Magic"  },
        { 18, "Double Trouble",  "Magic"  },
        { 19, "Tomax",           "Magic"  },
        { 20, "Stump Smash",     "Life"   },
        { 21, "Zook",            "Life"   },
        { 22, "Stealth Elf",     "Life"   },
        { 23, "Camo",            "Life"   },
        { 24, "Ghost Roaster",   "Undead" },
        { 25, "Hex",             "Undead" },
        { 26, "Chop Chop",       "Undead" },
        { 27, "Cynder",          "Undead" },
        { 28, "Boomer",          "Tech"   },
        { 29, "Drill Sergeant",  "Tech"   },
        { 30, "Trigger Happy",   "Tech"   },
        { 31, "Drobot",          "Tech"   },
        /* Giants */
        { 32, "Tree Rex",        "Life"   },
        { 33, "Bouncer",         "Tech"   },
        { 34, "Swarm",           "Air"    },
        { 35, "Crusher",         "Earth"  },
        { 36, "Thumpback",       "Water"  },
        { 37, "Ninjini",         "Magic"  },
        { 38, "Hot Head",        "Fire"   },
        { 39, "Eye-Brawl",       "Undead" },
        /* Swap Force originals */
        { 40, "Blast Zone",      "Fire"   },
        { 41, "Free Ranger",     "Air"    },
        { 42, "Rubble Rouser",   "Earth"  },
        { 43, "Doom Stone",      "Earth"  },
        { 44, "Wash Buckler",    "Water"  },
        { 45, "Boom Jet",        "Air"    },
        { 46, "Fire Kraken",     "Fire"   },
        { 47, "Stink Bomb",      "Life"   },
        { 48, "Grilla Drilla",   "Life"   },
        { 49, "Hoot Loop",       "Magic"  },
        { 50, "Trap Shadow",     "Magic"  },
        { 51, "Magna Charge",    "Tech"   },
        { 52, "Spy Rise",        "Tech"   },
        { 53, "Night Shift",     "Undead" },
        { 54, "Rattle Shake",    "Undead" },
        { 55, "Freeze Blade",    "Water"  },
        { 56, "Dune Bug",        "Magic"  },
        { 57, "Punk Shock",      "Water"  },
        { 58, "Battle Hammer",   "Life"   },
        { 59, "Sky Slicer",      "Air"    },
        { 60, "Slobber Tooth",   "Earth"  },
        { 61, "Scorp",           "Earth"  },
        { 62, "Fryno",           "Fire"   },
        { 63, "Smolderdash",     "Fire"   },
        { 64, "Bumble Blast",    "Life"   },
        { 65, "Zoo Lou",         "Life"   },
        { 66, "Chill",           "Water"  },
        { 67, "Gill Runt",       "Water"  },
        /* Trap Team */
        { 68, "Snap Shot",       "Water"  },
        { 69, "Lob Star",        "Water"  },
        { 70, "Flip Wreck",      "Water"  },
        { 71, "Echo",            "Water"  },
        { 72, "Blades",          "Air"    },
        { 73, "Fling Kong",      "Air"    },
        { 74, "Gusto",           "Air"    },
        { 75, "Thunderbolt",     "Air"    },
        { 76, "Food Fight",      "Life"   },
        { 77, "High Five",       "Life"   },
        { 78, "Krypt King",      "Undead" },
        { 79, "Short Cut",       "Undead" },
        { 80, "Funny Bone",      "Undead" },
        { 81, "Bat Spin",        "Undead" },
        { 82, "Jawbreaker",      "Tech"   },
        { 83, "Tread Head",      "Tech"   },
        { 84, "Gearshift",       "Tech"   },
        { 85, "Chopper",         "Tech"   },
        { 86, "Torch",           "Fire"   },
        { 87, "Ka-Boom",         "Fire"   },
        { 88, "Wildfire",        "Fire"   },
        { 89, "Trail Blazer",    "Fire"   },
        { 90, "Wallop",          "Earth"  },
        { 91, "Head Rush",       "Earth"  },
        { 92, "Fist Bump",       "Earth"  },
        { 93, "Rocky Roll",      "Earth"  },
        { 94, "Spotlight",       "Light"  },
        { 95, "Knight Light",    "Light"  },
        { 96, "Blackout",        "Dark"   },
        { 97, "Knight Mare",     "Dark"   },
        /* SuperChargers */
        { 98, "Fiesta",          "Undead" },
        { 99, "Spitfire",        "Fire"   },
        {100, "Stormblade",      "Air"    },
        {101, "Smash Hit",       "Earth"  },
        {102, "Spitfire (SC)",   "Fire"   },
        {103, "Dive-Clops",      "Water"  },
        {104, "Astroblast",      "Light"  },
        {105, "Nightfall",       "Dark"   },
        {106, "Thrillipede",     "Life"   },
        /* Imaginators */
        {108, "King Pen",        "Water"  },
        {109, "Tri-Tip",         "Earth"  },
        {110, "Chopscotch",      "Air"    },
        {111, "Boom Bloom",      "Life"   },
        {112, "Pit Boss",        "Undead" },
        {113, "Barbella",        "Tech"   },
        {114, "Air Strike",      "Air"    },
        {115, "Ember",           "Fire"   },
        {116, "Ambush",          "Life"   },
        {117, "Dr. Krankcase",   "Tech"   },
        {118, "Hood Sickle",     "Undead" },
        {119, "Tae Kwon Crow",   "Magic"  },
    };
    /* clang-format on */
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (table[i].id == id) return table[i].name;
    }
    return NULL;
}

static inline const char *skylander_element_from_id(uint16_t id) {
    static const skylander_info_t table[] = {
        {0,"","Air"},{1,"","Air"},{2,"","Air"},{3,"","Air"},
        {4,"","Earth"},{5,"","Earth"},{6,"","Earth"},{7,"","Earth"},
        {8,"","Fire"},{9,"","Fire"},{10,"","Fire"},{11,"","Fire"},
        {12,"","Water"},{13,"","Water"},{14,"","Water"},{15,"","Water"},
        {16,"","Magic"},{17,"","Magic"},{18,"","Magic"},{19,"","Magic"},
        {20,"","Life"},{21,"","Life"},{22,"","Life"},{23,"","Life"},
        {24,"","Undead"},{25,"","Undead"},{26,"","Undead"},{27,"","Undead"},
        {28,"","Tech"},{29,"","Tech"},{30,"","Tech"},{31,"","Tech"},
        {32,"","Life"},{33,"","Tech"},{34,"","Air"},{35,"","Earth"},
        {36,"","Water"},{37,"","Magic"},{38,"","Fire"},{39,"","Undead"},
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (table[i].id == id) return table[i].element;
    }
    return "Unknown";
}

/* Read the character ID from a decrypted 1024-byte Skylander dump.
 * Character ID is at offset 0x11 (bytes 17-18, little-endian). */
static inline uint16_t skylander_read_char_id(const uint8_t *data) {
    return (uint16_t)(data[0x11] | (data[0x12] << 8));
}

#endif /* SKYLANDER_IDS_H */
