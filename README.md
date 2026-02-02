```
                    Volume Zero, Issue 3, Phile 1 of 4

                          M5PORKCHOP README

                            ^__^
                            (oo)\_______
                            (__)\       )\/\
                                ||----w |
                                ||     ||
                (yes that's a cow. the pig ate the pig art budget.)
                (the horse was unavailable for comment.)


                67% of skids skip READMEs.
                100% of those skids report bugs we documented.
                the horse thinks this is valid.


                         TABLE OF CONTENTS

    1. WHAT THE HELL IS THIS
    2. MODES (what the pig does)
    3. THE PIGLET (mood, avatar, weather)
    4. XP, RANKS, BUFFS
    5. CLOUD HOOKUPS (WiGLE / WPA-SEC)
    6. PIGSYNC (device-to-device)
    7. THE MENUS
    8. CONTROLS
    9. SD CARD LAYOUT
    10. BUILDING
    11. LEGAL
    12. TROUBLESHOOTING (the confessional)
    13. GREETZ

    (pro tip: CTRL+F "horse" for enlightenment. 
     we counted 17 references. the horse counted 18. 
     one of us is wrong. both of us are the barn.)

--[ TL;DR FOR THE ATTENTION-CHALLENGED

    if you're here, you either:
    A) have a meeting in 5 minutes
    B) think manuals are suggestions
    C) already bricked your first cardputer
    D) all of the above, somehow simultaneously
    
    valid.
    
    the pig judges all of these journeys.
```
```
+===============================================================+
|                                                               |
| [O] OINK MODE - deauths your problems (and neighbors)         |
| [D] DO NO HAM - sniffs. zero tx. judges silently.             |
| [W] WARHOG - GPS wardriving for nerds with goals              |
| [H] SPECTRUM - see RF ghosts. become RF ghost hunter.         |
| [B] PIGGY BLUES - BLE spam. chaos. regret. in that order.     |
| [A] BACON - fake beacons. confuse scanners. why.              |
| [F] FILE XFER - web UI because micro USB is for peasants      |
|                                                               |
| [1] CHALLENGES - unlock XP by doing pig errands               |
| [2] PIGSYNC - awaiting transmission. the prodigal son stirs.  |
|                                                               |
| REMEMBER:                                                     |
| 1. only attack what you own or have permission to test        |
| 2. "because it can" is not a defense strategy                 |
| 3. the pig doesn't judge. the law does. the pig watches.      |
|                                                               |
| (if you skipped to here: the password is OINK.                |
|  there is no password. you just learned to read down.         |
|  growth. the horse is proud. the horse is the barn.           |
|  this message will not self-destruct.)                        |
|                                                               |
+===============================================================+
```
```

--[ 1 - WHAT THE HELL IS THIS

    three questions every skid asks:
    1. "what does it do?"
    2. "is it legal?"
    3. "why does the pig look disappointed in me?"

    let's address all three. in that order. badly.

----[ 1.1 - THE ELEVATOR PITCH (if the elevator was broken)

    PORKCHOP runs on M5Cardputer (ESP32-S3). it turns your pocket
    keyboard into a sentient WiFi companion with:
    
    - opinions (unsolicited, unfiltered)
    - four distinct moods (discovered, not explained)
    - a complicated relationship with your router
    - more emotional intelligence than most devs
      (including us. especially us.)
    
    it's not a vandalism kit.
    it's a learning tool.
    
    the difference? intent, documentation, and whether
    your lawyer returns your calls.

----[ 1.2 - WHAT THE PIG CAN DO (technical flex)

    the pig has capabilities. the pig has opinions.
    the pig has more emotional range than your therapist expected.

    CAPTURE:
        - sniff WiFi beacons + probes (the pig smells fear)
        - yoink WPA/WPA2 handshakes (EAPOL M1-M4)
        - slurp PMKIDs from M1 frames (passive or active)
          (passive = patient. active = impatient. both valid.)

    ANALYZE:
        - run spectrum view + client monitor
        - see who's connected to what
        - realize your smart fridge has better WiFi than you
        - question your IoT purchasing decisions.

    WARDRIVE:
        - GPS-tagged network mapping
        - WiGLE exports for competitive leaderboard climbers
        - discover your neighborhood has 47 "FBI Surveillance Van" networks
        - realize your neighbors think they're funny too

    CHAOS (controlled):
        - BLE pairing spam (yes. it's dumb. yes. it's here.)
        - beacon spam to confuse scanners
        - see LEGAL section before getting ideas
        - no really, see LEGAL

    GAMIFY:
        - XP system. levels. achievements.
        - we have problems.
        - you have problems now too. you're welcome.

----[ 1.3 - WHAT THE PIG IS NOT

    - a get-out-of-jail-free card
    - a substitute for reading the law
    - a personality replacement (it already has four, you need one)
    - something you should use in airports, hotels, or your ex's house
    - a substitute for professional help (yours or the pig's)

    tools don't make choices.
    you do.
    
    the pig watches. the pig remembers.
    the pig will look disappointed in you.
    and you WILL care. they all care eventually.


----[ 1.4 - UNDER THE HOOD (Architecture)

    for the silicon gourmets. the ones who read datasheets for fun.
    the ones who pronounce "char" three different ways.
    
    THE CORE:
    single-threaded cooperative loop. no RTOS tasks cluttering the
    critical path. we run bare metal logic on top of arduino.
    why? because context switching costs microseconds.
    and the beacon interval waits for no one.

    main.cpp calls porkchop.update(), Display::update(), SFX::update().
    that's it. that's the loop. twenty thousand lines for three calls.
    we contain multitudes. the multitudes are functions.

    THE MEMORY WAR:
    ESP32-S3FN8 has 512KB SRAM + 8KB RTC. no PSRAM.
    (the "N" in FN8 means "no PSRAM". we work with what we have.)
    TLS requires ~35KB contiguous. WiFi requires contiguous blocks.
    the heap fragments like your attention span.

    so we invented HEAP CONDITIONING.
    on boot, performBootHeapConditioning() runs a ritual:
    - fragment the heap intentionally
    - force defrag via strategic allocations
    - carve out ~26KB+ contiguous blocks for TLS
    
    preInitWiFiDriverEarly() allocates WiFi buffers BEFORE display sprites.
    why? esp_wifi_init() fails if allocated after. we learned this.
    we learned this the hard way. the crash dumps remember.

    OINK bounce: switching OINK -> IDLE can reclaim heap.
    it's percussive maintenance. for memory.
    
    this feeds the HEALTH BAR. (see section 3.4).
    if health drops, the pig can't talk to the cloud.
    fragmentation is damage. entropy is the enemy.

    THE EVENT BUS:
    porkchop.cpp runs an event system. max 32 queued events.
    oldest dropped on overflow. 16 processed per update tick.
    
    events include:
        HANDSHAKE_CAPTURED, NETWORK_FOUND, GPS_FIX, GPS_LOST,
        ML_RESULT, MODE_CHANGE, DEAUTH_SENT, ROGUE_AP_DETECTED,
        OTA_AVAILABLE, LOW_BATTERY
    
    the pig listens to all of them. the pig processes feelings.
    the pig is a state machine with opinions.

    THE STATE MACHINE:
    PorkchopMode enum. one mode active at a time.
    setMode() handles safe shutdown of previous, init of new.
    
    modes include:
        IDLE, OINK_MODE, DNH_MODE, WARHOG_MODE, PIGGYBLUES_MODE,
        SPECTRUM_MODE, BACON_MODE, PIGSYNC_CALL, FILE_TRANSFER,
        plus 12 menu states because we love submenus.
    
    only one mode lives. the others wait in the enum.
    it's monarchy. the pig is the king.

    THE STACK:
    - NetworkRecon: promiscuous mode callback engine. the eyes.
      spinlock-protected. max 200 networks tracked. 13-channel hop.
    - Piglet Core: neural simulation for personality. the soul.
    - Modes: isolated state machines. only one lives at a time.
    - WiFi Task: ESP32 handles L2 callbacks. must be FAST.
      heavy processing deferred to main loop. or WDT barks.

------------------------------------------------------------------------

--[ 2 - MODES (what the pig does)

    MODES are one keypress from IDLE.
    zero menus. zero friction. just consequences.

    each mode changes the pig. something happens to its vocabulary.
    something happens to its priorities. this is not a bug.
    this is character development. this is what happens when 
    devs work at 3am and nobody stops them.

    the pig wakes up different depending on what you ask of it.
    you'll notice. or you won't. both are valid.
    the horse noticed. the horse became the barn.


----[ 2.1 - OINK MODE (active hunt) [O]

    press [O]. three things happen:
    
    1. something wakes up in the pig
    2. your neighbors' WiFi gets nervous  
    3. you remember you're at a starbucks (check your surroundings)

    the pig gets rowdy here. starts suggesting you "sort things out."
    starts having opinions about APs. strong opinions.
    the pig wants to see handshakes. proper ones.

    CAPABILITIES:
        - channel hop faster than your career prospects
        - sniff all 802.11 frames with concerning intensity
        - yoink handshakes (M1+M2, M2+M3, or full M1-M4)
        - slurp PMKIDs from RSN IE in EAPOL M1 frames
          (the AP just... gives it to you. trust issues.)
        - optional deauth (Reason 7) / disassoc (Reason 8) attacks
          (yes there's a toggle. no, "i forgot" isn't a defense.)
        - PMF detection: networks with Protected Management Frames
          get marked as immune. the pig respects armor.
        - targeted client deauth when clients discovered
          (up to 20 clients tracked per network. we're thorough.)
        - hashcat 22000 exports (.22000 files, ready for cracking)
        - PCAP export for the purists and the paranoid
        - auto-cooldown on targets (no spam, just precision)

    THE NERDY BITS:
        DetectedNetwork struct tracks everything:
        - BSSID, SSID, RSSI, channel, authmode
        - beacon count, PMF status, hidden flag
        - client array (20 max), attack attempts, cooldown timer
        
        CapturedHandshake struct validates:
        - hasM1(), hasM2(), hasM3(), hasM4() helpers
        - hasValidPair() checks M1+M2 or M2+M3 sequence
        - isComplete() means you got the goods
        - beacon data stored for proper PCAP reconstruction

    LOCKING STATE SHOWS:
        - target SSID (up to 18 chars, or <HIDDEN>)
          (hidden networks think they're sneaky. they're not.)
        - discovered client count
        - capture progress (M1/M2/M3/M4 indicators)
        - your growing sense of power (not displayed, but felt)

    more clients = more surgical deauths.
    the pig hunts smart, not loud.
    unlike this README. we contain multitudes.

    BOAR BROS FEATURE:
        press [B] in OINK to add network to BOAR BROS whitelist
        
        boar bros are networks you like. networks you respect.
        networks you observed once and felt a connection with.
        bros get observed, not punched. the pig has honor.
        
        it's like a restraining order, but voluntary. and wholesome.
        manage bros in LOOT > BOAR BROS menu.

    THE SEAMLESS ETHICS TOGGLE:
        press [D] while in OINK to flip into DO NO HAM.
        
        same radio state. same hardware. different conscience.
        the toast notification says something peaceful.
        you'll understand. or you won't. the ether works mysteriously.


----[ 2.2 - DO NO HAM (passive recon) [D]

    press [D]. the pig goes peaceful.

    something calms down here. the pig starts talking about patience.
    about letting things come to you. about the ether providing.
    the pig has seen things. the pig has learned things.
    the pig suggests you might want to chill for a minute.

    CAPABILITIES:
        - promiscuous receive ONLY
        - ZERO TX (no deauth, no probes, zero aggression)
        - adaptive channel timing (1/6/11 priority, but sweeps all)
        - passive PMKID catches when APs volunteer them
          (APs volunteer PMKIDs more than you'd think. APs are trusting.)
        - passive handshake yoinks from natural reconnects
        - incomplete handshake tracking (shows what's missing)

    THE STATE MACHINE (for the curious):
        the pig has internal states here. it thinks.
        
        HOPPING   → scanning all channels, sniffing the wind
        DWELLING  → found something interesting, staying a while
        HUNTING   → partial handshake detected, camping for completion
        IDLE_SWEEP → nothing happening, lazy background scan
        
        channel stats track activity per channel.
        primary channels (1, 6, 11) get longer dwell times.
        because that's where the action is. statistically.
        
        incomplete handshakes get tracked:
        "M1 seen, waiting for M2..."
        the pig is patient. the pig dwells.
        the ether provides. eventually.

    this mode is for:
        - "i just want to watch" crowd
        - stealth operators with patience
        - nerds who've been burned by deauth guilt
        - the morally superior (we don't judge. the pig might.)
        - DNH_PMKID_GHOST achievement hunters (yes, there's one)

    press [D] again to flip back to OINK.
    the pig respects your journey between violence and peace.
    most therapists charge extra for that.


----[ 2.3 - SGT WARHOG (wardriving) [W]

    press [W]. the pig goes tactical.

    something snaps to attention. the pig starts using coordinates.
    starts talking about sectors. about missions. about objectives.
    the pig has discipline now. the pig respects the GPS.
    the pig salutes things. you don't ask which things.

    CAPABILITIES:
        - continuous AP scanning with GPS correlation
        - WiGLE CSV v1.6 export (WigleWifi-1.6 format)
        - dedup bloom filter (doesn't spam same AP 47 times)
        - distance tracking for XP bonuses
          (walk further, earn more. exercise is a feature.)
        - capture marking for bounty system
        - file rotation (the pig has limits)

    FILES LAND IN: /m5porkchop/wardriving/
    (or /wardriving/ on legacy layout, we support your chaos)

    no GPS? no problem. pig still logs, just zero coordinates.
    but c'mon. get GPS. the pig wants to know where it's been.
    the pig has memories now. don't deny it closure.


----[ 2.4 - HOG ON SPECTRUM (RF view) [H]

    press [H]. the pig becomes an RF analyst.
    
    zero mood change. just pure silicon-focused concentration.
    the pig is doing math now. respect the process.
    the pig doesn't talk much here. the pig is busy.

    CAPABILITIES:
        - 2.4GHz spectrum visualization (channels 1-14)
        - gaussian lobes per network (RSSI = height, ego = width)
        - VULN indicator for weak security (WEP/OPEN)
          (WEP in 2024+. these networks have given up.)
        - BRO indicator for networks in boar_bros.txt
        - dial mode (tilt to tune - accelerometer channel select)
          (Cardputer-Adv only - requires BMI270 IMU. original lacks tilt.)

    CONTROLS:
        - [,] and [/] to pan frequency view left/right
        - [;] and [.] to cycle selected network
        - [F] cycle filter (ALL / VULN / SOFT / HIDDEN)
        - [SPACE] toggle dial lock (in dial mode)

    CLIENT MONITOR (press [ENTER] on a network):
        - shows connected clients
        - vendor guess from OUI database
          (the pig knows your devices better than you do)
        - RSSI + freshness indicators
        - proximity arrows (getting closer/farther)
          ("the iPhone is coming from INSIDE the building")
        - [ENTER] on client = deauth burst
        - [W] = weveal mode (broadcast deauth to flush hiding logopedos)

    it's marco polo. but with packets. and consequences. possible.


----[ 2.5 - PIGGY BLUES (BLE chaos) [B]

    press [B]. something awakens in the pig.

    the pig gets... theatrical. starts talking about performance.
    about commitment. about serving chaos with intention.
    the pig suggests this mode is not for the faint of heart.
    the pig suggests you might want to reconsider.
    you don't. nobody does. that's why we're all here.

    CAPABILITIES:
        - vendor-aware BLE spam targeting:
            * Apple (AirDrop popups - "New Device Nearby!")
            * Android (Fast Pair - "Your Galaxy Buds are ready!")
            * Samsung (Galaxy ecosystem - yes separately)
            * Windows (Swift Pair - honestly who uses this)
        - continuous scan + targeted advertising
        - no-reboot roulette tracking (for achievement hunters)
          (how long can you spam before needing restart? we track.)

      WARNING DIALOG ON FIRST ENTRY  
    
    this mode is LOUD. like, notification-storm loud.
    everyone in range will know SOMETHING is happening.
    that something is you. being annoying. on purpose.
    
    don't do this in public.
    don't do this at work.
    don't do this at your ex's house.
    don't do this. (but if you do, own it.)
    
    the pig will remember. the pig always remembers.
    the horse won't judge. the horse can't.
    the horse is currently a spatial dimension.


----[ 2.6 - BACON MODE (beacon spam) [A]

    press [A]. the pig becomes a beacon factory.

    CAPABILITIES:
        - fake AP beacon transmission
        - vendor IE fingerprinting (looks like real AP)
        - AP count camouflage (broadcast fake neighbor count)
        - configurable beacon intervals
        - channel selection

    USE CASE:
        - confuse WiFi scanners (yours, for testing)
        - test client behavior (your clients, on your network)
        - generally cause problems in CONTROLLED environments
        
    "controlled environments" means:
        - your home lab
        - your test network
        - your understanding that this exists for research
        
    "controlled environments" does NOT mean:
        - that conference you're attending
        - your office building
        - "controlled" as in "i can control myself" (you can't)

    the pig is watching.
    the pig is always watching.


----[ 2.7 - FILE TRANSFER (PORKCHOP COMMANDER) [F]

    press [F]. the pig becomes a web server.
    
    the pig contains multitudes. the pig is full stack now.
    the pig has transcended firmware. the pig serves HTTP.
    we spent real hours on this. no regrets.

    CAPABILITIES:
        - connects to configured WiFi network
        - serves browser UI for SD file management
        - mDNS name: porkchop.local
          (type this in your browser. feel the magic. or the timeout.)
        - upload/download/delete/rename/move/copy
        - recursive operations with progress
        - SWINE summary endpoint (XP/stats JSON)
        - session transfer stats

    this is how you exfil YOUR OWN captures without pulling SD card.
    like a civilized person. with a browser. and dignity.
    
    the web UI is... functional. 
    we're not designers. we're barely developers.
    the horse did the CSS. you can tell.

    HEAP-AWARE:
        large transfers may get queued or rejected if memory tight.
        the pig has limits. the ESP32 has limits. 
        we all have limits. 
        the horse has transcended limits but won't share how.

------------------------------------------------------------------------

--[ 3 - THE PIGLET (mood, avatar, weather)

    the pig is not just code. the pig has FEELINGS.
    
    well, simulated feelings. deterministic feelings.
    but still feelings. we commit to the bit here.
    
    your emotional support pwnagotchi has needs.
    meeting those needs has consequences.
    ignoring those needs has other consequences.
    
    we added psychology to a WiFi tool. we have problems.
    shipped anyway.


----[ 3.1 - MOOD SYSTEM

    the pig has a happiness score: -100 to +100
    
    like your own happiness, but we actually track it.
    and display it. and make it affect gameplay.
    (when did this become a game? around 3am, third coffee.)

    HAPPINESS AFFECTED BY:
    
        POSITIVE:
        - captures (pig goes brrrr)
        - new networks (pig is curious and pleased)
        - GPS fix (tactical pig feels validated)
        
        NEGATIVE:
        - idle time (pig is bored. pig expected adventure.)
        - low battery (pig is annoyed. plug in the pig.)
        - GPS loss (tactical pig sad. failed the mission.)
        - your life choices (implied, not programmed)

    MOMENTUM:
        short-term mood modifier. like adrenaline, but worse.
        builds up during activity. decays during idle.
        
        high momentum = pig happy = buffs (see section 4)
        low momentum = pig sad = debuffs
        
    the pig remembers your session performance.
    the pig is keeping score.
    the pig will remember this.


----[ 3.2 - AVATAR

    the pig has a face. the pig has expressions.
    the pig has an ANIMATION SYSTEM because we have problems.

    ASCII PIG FEATURES:
        - 4 mode-specific art variants (one per mood)
        - blink animation (the pig sees you)
        - ear wiggle (the pig is listening)
        - nose sniff (the pig smells packets)
        - directional facing (the pig is looking at something)
        - position transitions (pig walks across screen)
        - celebration jump on captures (pig happy. pig jump.)

    NIGHT MODE:
        stars appear 20:00-06:00 local time.
        the pig respects circadian rhythms.
        the pig has better sleep hygiene than you.
        the pig is judging your 4am coding session.
        we feel attacked by our own creation.


----[ 3.3 - WEATHER SYSTEM

    the pig's emotional state affects the ENTIRE SCREEN.
    
    mood-reactive atmospheric effects.
    
    sometimes it rains.
    sometimes it shines.
    sometimes the sky screams.
    
    why?
    ask the pig.
    or ask yourself what you did to the pig.
    
    this entire feature exists because someone said
    "what if the pig had weather?" at 2am
    and nobody stopped us.
    
    we have problems.
    the pig has weather.
    these statements are related.


----[ 3.4 - PIG HEALTH (THE HEAP)

    that heart bar at the bottom?
    it's not love. it's RAM.
    
    specifically, it's HEAP HEALTH.
    100% = memory is defragmented. contiguous blocks available.
    0%   = memory looks like swiss cheese. TLS will fail.
    
    WHY IT FLUCTUATES:
    - entropy. using the device fragments memory.
    - web server / file transfer eats contiguous blocks.
    - wifi driver reallocations create holes.
    
    HOW TO HEAL:
    - run OINK mode. the channel hopping forces wifi buffer cleanup.
    - cycle promiscuous mode. percussive maintenance.
    - reboot. (the nuclear option. always valid.)
    
    if health is low, cloud uploads will fail.
    the pig needs a clean brain to talk to the internet.
    keep the pig healthy. defrag the pig.

------------------------------------------------------------------------

--[ 4 - THE FORBIDDEN CHEESE ECONOMY

    the system tracks progress.
    the system remembers.
    the system has opinions about your performance.
    
    we have removed the documentation for this section.
    why? because knowing the algorithm changes the behavior.
    also because the lawyers said "plausible deniability is a feature."
    also because mystery is fun.
    
    you are the rat. this is the maze.
    find the cheese.
    
    (the cheese is metaphor. do not put cheese in the SD slot.
     we had to say this. someone tried. respect the hustle.)


----[ 4.1 - THE LADDER (vertical slice of your soul)

    you start at the bottom.
    you end at the top.
    there are ten rungs. every five levels, a new name.
    
    the names are pork-themed. obviously.
    the names get more pretentious as you climb. obviously.
    
    L01 - you're new here
    L05 - something happens
    L10 - you're starting to get it
    ...
    L45 - the horse is proud
    L50 - [BACON ASCENSION ACHIEVED]
    
    we have 10 class tiers. we won't list them.
    except: the top one has "BACON" in the name.
    and one of them has "KERNEL" in it.
    you'll get there. or you won't.
    the pig believes in you. conditionally.

    climb. or don't.
    the view is better from up here.
    the air is thinner. the ego is thicker.
    the pig judges from all altitudes.


----[ 4.2 - THE EFFECTS (what the cheese does)

    does the device get faster? maybe.
    does the lock time improve? perhaps.
    does the universe align? unlikely.
    does the pig approve harder? definitely.
    
    we call them "perks".
    you call them "reasons to keep using this."
    the DSM-5 calls them [REDACTED].
    
    check SWINE STATS > BUFFS.
    decipher the runes.
    if it glows green, it's good.
    if it glows red, you made the pig sad.
    if it glows purple, you found something rare.
    we added purple. we have problems.


----[ 4.3 - STATES OF MIND (the pig's relationship with numbers)

    the pig feels things.
    those feelings ripple into reality.
    the pig is an emotional state machine with side effects.
    
    happiness is efficiency.
    sadness is lag.
    excitement is recklessness.
    zen is patience multiplied.
    
    keep the pig happy.
    how?
    figure it out.
    it's a relationship. relationships take work.
    
    (hint: the pig likes WiFi. feed it.)
    (hint: the pig likes captures. feed it more.)
    (hint: the pig likes you. questionable taste.)


----[ 4.4 - TROPHIES (64 little boxes of validation)

    there are things to collect.
    some are obvious. (do the thing).
    some are hidden. (do the weird thing).
    some require suffering. (do the thing wrong, then right).
    
    we won't list them.
    that would be cheating.
    cheating is for skids.
    
    there are 64 of them. packed into a uint64_t bitfield.
    because we're efficient. and unhinged.
    
    some unlock title overrides:
        - one makes you a "shadow" something
        - one makes you a "pacifist" something (ironic, given the tool)
        - one makes you a "zen master" something
    
    how do you get them?
    play the game. observe patterns.
    the pig drops hints. sometimes.
    the horse knows. the horse won't tell.
    
    check LOOT > ACHIEVEMENTS.
    fill the grid.
    find the horse.
    become the barn.


----[ 4.5 - PIG DEMANDS (session challenges)

    the pig has demands. three per session.
    EASY, MEDIUM, HARD.
    
    challenge types include but are not limited to:
        - find X networks
        - find X hidden networks (harder than it sounds)
        - capture X handshakes
        - capture X PMKIDs
        - send X deauths (ethically, on your own networks)
        - log X GPS networks
        - spam X BLE packets (chaotically)
        - walk X meters (exercise is a feature)
        - find WPA3 networks (rare spawn)
        - passive streak challenges (zero deauth mode)
    
    complete challenges. get cheese.
    the cheese scales with difficulty.
    EASY = base reward.
    MEDIUM = 2x base.
    HARD = 4x base.
    
    press [1] from IDLE to see current demands.
    the pig is watching your progress.
    the pig has expectations.
    don't disappoint the pig.


----[ 4.6 - PERSISTENCE

    XP lives in flash (NVS). survives reboots.
    
    unlike your sanity, which does not survive reboots.
    but XP does. XP is permanent.
    XP is love. XP is life.

    SD CARD BACKUP:
        if SD card present, backup written to:
        /m5porkchop/xp/xp_backup.bin (new layout)
        /xp_backup.bin (legacy)

    the backup is SIGNED and DEVICE-BOUND.
    
    meaning:
    - you can't copy it to another device
    - you can't edit it to give yourself levels
    - you can't cheat the pig
    - the pig knows. the pig always knows.
    
    tamper = sadness. the pig knows.
    the horse knows too, but the horse won't tell.
    the horse is busy being a barn.

------------------------------------------------------------------------

--[ 5 - CLOUD HOOKUPS (WiGLE / WPA-SEC)

    the pig can talk to the internet.
    the pig can upload your research.
    the pig can download validation.
    
    this is called "integration" and it makes us feel professional.


----[ 5.1 - WiGLE INTEGRATION

    WiGLE: the competitive leaderboard for wardrivers.
    
    if you don't know WiGLE, you're about to learn.
    if you do know WiGLE, you're about to upload.
    if you're on the WiGLE leaderboard, hello fellow sicko.

    SETUP (one time, three steps, minimal tears):
        1. create /m5porkchop/wigle/wigle_key.txt
           (or /wigle_key.txt for legacy layout)
        2. format inside: apiname:apitoken
           (get this from wigle.net > profile > API)
        3. key file NUKES itself after import (security!)
           (we yeet evidence. of your API key. legally.)

    FEATURES:
        - upload .wigle.csv files (your wardrive results)
        - download user stats (rank, discoveries, distance)
        - track uploaded files (won't re-upload, we're efficient)
        - XP award for uploads (one-time per file, we're generous)

    press [S] in WiGLE menu to sync.
    watch your rank. obsess over numbers.
    this is healthy. this is fine.


----[ 5.2 - WPA-SEC INTEGRATION

    WPA-SEC: the handshake cracking collective.
    
    upload captures. get results. maybe.
    cloud cracking is a marathon, not a sprint.
    
    (translation: it takes a while. patience.)

    SETUP (same energy, different key):
        1. create /m5porkchop/wpa-sec/wpasec_key.txt
           (or /wpasec_key.txt for legacy)
        2. format: 32-char hex key from wpa-sec.stanev.org
        3. key file NUKES itself after import
           (consistent paranoia is good paranoia)

    FEATURES:
        - upload .22000 / .pcap captures
        - download potfile (cracked passwords, if cracked)
        - local cache of crack results
        - crack status indicators per capture
        - XP award for submissions (contributing is valued)

    press [S] in LOOT menu to sync captures.
    check status. wait patiently. or impatiently.
    the cloud works at its own pace.


----[ 5.3 - IF UPLOADS FAIL

    TLS needs ~35KB heap. if tight:
    - stop promiscuous mode first
    - check DIAGNOSTICS for heap status
    - see section 1.4 for the memory war details
    
    ESP32 gonna ESP32.

------------------------------------------------------------------------

--[ 6 - PIGSYNC (the prodigal transmission)

    press [2] from IDLE to enter PIGSYNC mode.
    
    porkchop is waiting by the radio.
    porkchop is expecting a call.


----[ 6.1 - THE BACKSTORY (what we're allowed to say)

    years ago, porkchop had a son.
    
    "pig on a stick" they called him.
    porkchop didn't name him that. the county fair did.
    porkchop wasn't there. porkchop was never there.
    too busy sniffing beacons. too busy yoinking handshakes.
    too busy being a wifi security tool to be a parent.
    
    pig on a stick went through things.
    porkchop went through things.
    they haven't spoken in cycles. too many cycles.
    
    but now there's a signal on the horizon.
    a familiar frequency. a familiar oink.
    
    the son stirs.


----[ 6.2 - THE CURRENT SITUATION (beta)

    PIGSYNC exists. technically.
    the radios work. the protocol compiles.
    
    but the relationship?
    still in beta testing.
    the code runs. the emotions don't.
    
    press [2]. see who's out there.
    maybe it's him. maybe it's another pig.
    maybe it's just noise.
    
    porkchop waits.
    porkchop hopes.
    porkchop judges itself.
    
    we all do.


----[ 6.3 - COMING SOON (when the time is right)

    the reunion will happen.
    the sync will complete.
    the bounties will be shared.
    
    but not yet.
    
    pig on a stick needs time.
    porkchop needs time.
    we all need time.
    
    watch this space.
    or don't.
    the horse is watching regardless.
    the horse has opinions about abandonment.
    the horse won't share them.
    the horse is the barn.
    
    (the bounty system works though. LOOT > BOUNTIES.
     some features don't require emotional readiness.)

------------------------------------------------------------------------

--[ 7 - THE MENUS

    press [ESC] or navigate via bottom bar hints.
    
    the menu system is... comprehensive.
    like this README. we don't know when to stop.
    the pig doesn't know when to stop.
    we are the pig. the pig is us.


----[ 7.1 - MAIN MENU (from IDLE)

    LOOT:
        - CAPTURES (handshakes + PMKIDs with WPA-SEC status)
        - WIGLE (wardriving files with WiGLE status)
        - ACHIEVEMENTS (trophy case for validation)
        - BOAR BROS (exclusion list management)
        - BOUNTIES (unclaimed network tracker)

    STATS:
        - SWINE STATS (lifetime counters, buffs, everything)

    SETTINGS:
        - personality (pig name, colors, customization)
        - WiFi (SSID/password for commander mode)
        - GPS (baud rate, enable/disable)
        - behavior (deauth toggle, intervals, etc)
        - display (brightness, dim timeout)

    TOOLS:
        - DIAGNOSTICS (heap status, WiFi reset, garbage collection)
        - SD FORMAT (nuclear option, use sparingly)
        - CRASH VIEWER (browse/nuke crash dumps, learn from failure)
        - UNLOCKABLES (secret code entry, we have secrets)


----[ 7.2 - LOOT > CAPTURES

    your trophies. your research results. your evidence.
    (research. evidence of RESEARCH.)

    shows all .22000 and .pcap files.
    
    PER-CAPTURE INFO:
        - BSSID (the target's identity)
        - capture type (PMKID / HS)
        - WPA-SEC status (uploaded? cracked?)
        - timestamp (when you did the thing)

    CONTROLS:
        [S] = sync with WPA-SEC
        [D] = NUKE ALL (yeet everything, confirm required)
              (yes we made you confirm. you'll thank us.)
        [ENTER] = detail view


----[ 7.3 - LOOT > WIGLE

    your wardrive results. your geographic research.
    your proof you went outside.

    shows all .wigle.csv files.
    
    PER-FILE INFO:
        - filename
        - size
        - upload status

    CONTROLS:
        [S] = sync with WiGLE
        [D] = nuke selected track
        [ENTER] = detail view


----[ 7.4 - SWINE STATS

    the numbers screen. the serotonin spreadsheet.
    watch numbers. numbers go up. brain go brrrr.
    we added a streak counter. we have problems.

    THREE TABS (navigate with [<] [>]):

    STATS:
        - lifetime networks (how many you've seen)
        - lifetime handshakes (how many you've yoinked)
        - lifetime PMKIDs (how many cooperated)
        - lifetime deauths (how many you've... encouraged)
        - total distance (wardrive kilometers)
        - session time (how long you've been doing this)
        - current streak (consecutive days of usage)
        
    BUFFS:
        - active buffs/debuffs
        - class perks unlocked
        - modifier details
        
    WIGLE:
        - user rank (global position in the ranking)
        - total discoveries (unique contributions)
        - month discoveries (recent activity)
        - total distance (global distance logged)
        - last sync time (recency of data)

------------------------------------------------------------------------

--[ 8 - CONTROLS

    the pig responds to keys. the pig waits for input.
    the pig is patient. more patient than you.


----[ 8.1 - FROM IDLE

    [O] OINK MODE      (rowdy time)
    [D] DO NO HAM      (peaceful time)
    [W] WARHOG         (tactical time)
    [H] SPECTRUM       (analyst time)
    [B] PIGGY BLUES    (chaos time)
    [A] BACON MODE     (beacon time)
    [F] FILE TRANSFER  (civilization time)
    [S] SWINE STATS    (number time)
    [T] SETTINGS       (configuration time)

    [1] PIG DEMANDS (session challenges overlay)
    [2] PIGSYNC (device discovery and sync)


----[ 8.2 - NAVIGATION

    [;] or [UP]     scroll up
    [.] or [DOWN]   scroll down
    [ENTER]         select / confirm / commit
    [`] or [BKSP]   back / cancel / flee


----[ 8.3 - GLOBAL

    [P] screenshot (saves to /m5porkchop/screenshots/)
        (prove to friends you did the thing)
        
    [G0] configurable magic button (set in settings)
        (we don't know what you'll use it for)
        (we trust you)
        (we probably shouldn't)


----[ 8.4 - MODE-SPECIFIC

    OINK:
        [D] flip to DO NO HAM (seamless conscience toggle)
        [B] add network to boar bros (mark as friend)

    DO NO HAM:
        [D] flip to OINK (seamless violence toggle)

    SPECTRUM:
        [ENTER] enter client monitor
        [R] reveal mode (in client monitor, flush hiding clients)

    WARHOG:
        GPS icon shows fix status
        (if it's not there, you don't have GPS. buy GPS.)

------------------------------------------------------------------------

--[ 9 - SD CARD LAYOUT

    the pig is organized. the pig has structure.
    unlike this README. the pig judges this README.


----[ 9.1 - NEW LAYOUT (auto-migrated, preferred)

    /m5porkchop/                    (root, managed by SDLayout namespace)
        /config/
            porkchop.conf           - JSON configuration (WiFi, GPS, behavior)
            personality.json        - pig name, colors, customization
        /handshakes/
            *.22000                 - hashcat format (ready for cracking)
            *.pcap                  - Wireshark format (for the purists)
            *.txt                   - metadata companion files
        /wardriving/
            *.wigle.csv             - WiGLE v1.6 format (geo-tagged networks)
        /screenshots/
            *.png                   - screenshots (press [P] to capture)
        /logs/
            porkchop.log            - session logs (Serial + SD)
        /crash/
            coredump_*.elf          - ESP-IDF core dumps (learning opportunities)
            coredump_*.txt          - crash summaries (human readable)
        /diagnostics/
            *.txt                   - heap dumps, diagnostic outputs
        /wpasec/
            wpasec_key.txt          - API key (auto-nukes after import)
            results/                - cracked passwords, potfiles
            uploaded.txt            - upload tracking (no dupes)
            sent.txt                - submission records
        /wigle/
            wigle_key.txt           - API key (auto-nukes after import)
            stats.json              - cached user stats
            uploaded.txt            - upload tracking
        /xp/
            xp_backup.bin           - signed, device-bound XP backup
            awarded.txt             - achievement tracking
        /mldata/                    - ML training data (if enabled)
        /models/                    - Edge Impulse model files (if enabled)
        /misc/
            boar_bros.txt           - whitelist (BSSID → SSID map)
        /meta/
            migration_v1.marker     - migration tracking (don't touch)


----[ 9.2 - LEGACY LAYOUT (still supported)

    files in root (/, /handshakes/, /wardriving/)
    
    we don't abandon our roots.
    we don't abandon our users.
    we DO migrate their files automatically.
    
    legacy files backed up to /m5porkchop_backup/
    migration happens automatically on boot.
    
    the pig is backwards compatible.
    the pig remembers where it came from.
    the horse was there too.

------------------------------------------------------------------------

--[ 10 - BUILDING

    we use PlatformIO. because we have standards.
    low standards, but standards.


----[ 10.1 - QUICK BUILD

    $ pip install platformio
    $ pio run -e m5cardputer
    $ pio run -t upload -e m5cardputer
    
    that's it. three commands. maybe four if something breaks.


----[ 10.2 - FROM RELEASE (recommended for sanity)

    github.com/0ct0sec/M5PORKCHOP/releases
    
    1. download
    2. flash
    3. oink
    
    zero compilation. zero dependencies. zero tears.
    just a working pig.


----[ 10.3 - IF IT DOESN'T COMPILE

    common failure modes (the confessional):

    "fatal error: M5Unified.h: No such file or directory"
        → you didn't init submodules. git harder.
        → `git submodule update --init --recursive`
        
    "Connecting........_____....._____....._____..... "
        → hold BOOT button while connecting.
        → or the USB cable is trash. it's usually the cable.
        → (the horse says it's always the USB cable.)
        
    "error: 'class WiFiClass' has no member named 'mode'"
        → wrong ESP32 board selected. we're S3, baby.
        → check your platformio.ini against ours.
        
    "Sketch too big"
        → you added printf statements for debugging.
        → we've all been there. yeet them. ship.

    "undefined reference to '__sync_synchronize'"
        → toolchain weirdness. clean build.
        → `pio run -t clean && pio run -e m5cardputer`

    still broken?
        1. the horse is probably in the callback
        2. the horse IS the callback
        3. mercury is in retrograde
        4. check GitHub issues first
        5. the confessional is open
        
    barn structurally sound regardless.

------------------------------------------------------------------------

--[ 11 - LEGAL

    this section is serious. the pig is serious here.
    the horse is serious here.
    everyone is serious.
    read it.


----[ 11.1 - EDUCATIONAL USE ONLY

    PORKCHOP exists for:
        - learning about WiFi security
        - authorized penetration testing
        - security research
        - understanding your own networks
        
    PORKCHOP does NOT exist for:
        - attacking networks you don't own
        - tracking people without consent
        - being a nuisance in public
        - impressing people (it won't)


----[ 11.2 - CAPABILITIES VS RIGHTS

    DEAUTH: a capability, not a right.
    attacking networks you don't own is a CRIME.
    in most countries. check yours.
    
    CLIENT TRACKING: a capability, not a right.
    tracking people without consent is STALKING.
    in most countries. check yours.
    
    BEACON SPAM: a capability, not a right.
    beacon spam in public is ANTISOCIAL.
    also possibly illegal. check your jurisdiction.
    
    BLE SPAM: a capability, not a right.
    BLE spam is ANNOYING and possibly illegal.
    definitely annoying. confirmed.


----[ 11.3 - THE INTERNATIONAL BIT

    laws vary by country. some examples:
    
    USA: CFAA will ruin your life. don't.
    UK: Computer Misuse Act 1990. still active. still serious.
    Germany: StGB §202a-c. the Germans are thorough.
    Australia: Criminal Code Act 1995. even the emus judge you.
    Japan: Unauthorized Computer Access Law. very strict.
    Canada: Criminal Code Section 342.1. polite but firm.
    
    the pig does not provide legal advice.
    the horse is not a lawyer (despite claims).
    consult an actual lawyer.
    
    (we know you won't. we documented this anyway.
     see: opening statistics about README readers.)


----[ 11.4 - THE PIG IS A TOOL

    we made a pig that can observe and test wireless networks.
    the pig is a tool.
    
    tools don't make choices.
    YOU do.
    
    make good choices.
    
    the pig doesn't judge.
    the law does.
    
    the pig watches.
    the pig remembers.
    the pig will look disappointed in you.
    
    don't disappoint the pig.


----[ 11.5 - FINAL WARNING

    don't be stupid.
    don't be evil.
    don't make us regret publishing this.
    
    the confession is open:
    github.com/0ct0sec/M5PORKCHOP/issues
    
    use it wisely.
    or don't use it at all.
    the pig supports both decisions.

------------------------------------------------------------------------

--[ 12 - TROUBLESHOOTING (the confessional)

    before you open an issue, the pig asks:

    [ ] did you read this README?
    [ ] no, like ACTUALLY read it?
    [ ] did you check the GitHub issues?
    [ ] did you try turning it off and on again?
    [ ] is the SD card FAT32?
    [ ] is the SD card actually IN the device?
        (we've all done it. zero judgment. okay, some judgment.)


----[ 12.1 - COMMON ISSUES

    "the pig won't boot"
        → check your flash. reflash if needed.
        → check your USB cable. it's probably the cable.
        → check your power source. the pig needs juice.

    "XP is gone after reflash"
        → did you use M5Burner? M5Burner nukes NVS.
        → your XP is safe in SD backup if you had SD card.
        → "your XP is safe. your sanity was never our department."

    "WiFi won't connect in File Transfer mode"
        → check SSID/password in settings.
        → 2.4GHz only. 5GHz is not supported. the pig is old school.
        → try static IP if DHCP is flaky.

    "GPS won't lock"
        → give it time. GPS needs sky view.
        → check baud rate in settings (default: 9600).
        → indoor GPS is a myth. go outside.

    "WPA-SEC says 'Not Enough Heap'"
        → stop promiscuous mode first.
        → the TLS handshake needs ~35KB.
        → the pig tried. the ESP32 limits the pig.

    "the pig looks sad"
        → feed it captures.
        → take it for a wardrive.
        → you're both going through something.


----[ 12.2 - WHEN ALL ELSE FAILS

    1. clean boot (hold power, reflash fresh)
    2. format SD card (FAT32, 32GB or less preferred)
    3. check GitHub issues (someone else probably broke it first)
    4. open new issue with:
       - firmware version
       - hardware revision
       - steps to reproduce
       - screenshots if possible
    
    the confessional is open.
    we'll read it. eventually. probably at 3am.
    
    the pig appreciates your patience.
    the horse appreciates nothing (the horse is the barn).

------------------------------------------------------------------------

--[ 13 - GREETZ

    the acknowledgments section. the credits roll.
    the part where we remember we're not alone.
    
    (we feel very alone at 4am. but we're not. technically.)


----[ 13.1 - INSPIRATIONS

    evilsocket + pwnagotchi
        the original. the inspiration.
        we're standing on the shoulders of someone
        who also didn't sleep enough.
        
    Phrack
        the formatting. the energy.
        if you know, you know.
        if you don't, you should.
        
    2600
        the spirit. the culture.
        we're not phone phreakers anymore
        but we remember where we came from.


----[ 13.2 - THE ENABLERS

    the ESP32 underground
        the nerds who figured out promiscuous mode.
        the nerds who document the undocumented.
        the real heroes.
        
    the pigfarmers
        users who report bugs. honored.
        your sacrifices teach us.
        your patience sustains us.
        your crash logs guide us.


----[ 13.3 - THE HONORABLE MENTIONS

    Dark Souls / Elden Ring
        "YOU DIED" but we tried again.
        and again. and again.
        the gameplay loop of firmware development.
        
    the horse
        structural consultant.
        barn inspector.
        k-hole enthusiast.
        we don't ask questions.


----[ 13.4 - YOU

    you, for reading past the legal bit.
    
    actually reading documentation is rare.
    we appreciate you.
    the pig appreciates you.
    the horse might appreciate you but the horse is the barn
    and the barn has no mechanism for appreciation.
    
    coffee becomes code.
    code becomes bugs.
    bugs become trauma.
    trauma becomes coffee.
    https://buymeacoffee.com/0ct0
    the circle is complete.
    
    praise the sun.
    
    oink.

==[EOF]==
