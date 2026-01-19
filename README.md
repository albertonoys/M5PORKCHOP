--[ M5PORKCHOP

    pwnagotchi meets tamagotchi.
    but it's an ASCII pig.
    and it lives in an M5Cardputer.

    educational + authorized security research ONLY.


--[ Quick Start (the only way that matters)

    fetch a release. flash however you like. pig doesn't care.

        - github.com/0ct0sec/M5PORKCHOP/releases
        - download. flash. oink.

    XP/LEVEL/Achievements live in flash (NVS). if you have an SD card,
    the pig also writes a tiny backup blob to:

        /xp_backup.bin

    it's signed. device-bound. tamper = sadness.
    the pig remembers. oink.


--[ TL;DR FOR THE ATTENTION-CHALLENGED

```
+===============================================================+
|                                                               |
|   1) press [O] = OINK MODE (active hunt)                      |
|   2) press [D] = DO NO HAM (passive, no TX)                   |
|   3) press [W] = WARHOG (GPS wardrive + WiGLE export)         |
|   4) press [H] = SPECTRUM (RF reality + CLIENT MONITOR)       |
|   5) LOOT menu = see hashes + WPA-SEC status                  |
|                                                               |
|   only attack what you own or have permission to test.        |
|   "because it can" is not a defense.                          |
|                                                               |
+===============================================================+
```


--[ Contents

    1 - What the hell is this
    2 - Modes (what the pig does)
    3 - XP, ranks, buffs
    4 - Cloud hookups (WiGLE / WPA-SEC)
    5 - Controls
    6 - Building
    7 - Legal
    8 - Greetz


--[ 1 - What the hell is this

    PORKCHOP runs on M5Cardputer (ESP32-S3) and turns your pocket
    keyboard into a tiny WiFi recon companion.

    it's not a vandalism kit.
    it's a learning tool.
    precision, knowledge, ethics.
    romanticize the craft, not the crime.

    we made a pig that can:

        - sniff WiFi beacons + probes
        - capture WPA/WPA2 handshakes (EAPOL)
        - yoink PMKIDs from M1 frames (passive)
        - run a spectrum view + client monitor
        - wardrive with GPS + WiGLE exports
        - spam BLE pairing prompts (yes. it's dumb. yes. it's in here.)
        - level up like an RPG because we have problems

    tools don't make choices.
    you do.


--[ 2 - Modes (what the pig does)

    MODES are one keypress from IDLE.
    no menus needed.
    the pig likes speed.


----[ 2.1 - OINK MODE (active hunt)

    press [O]. pig goes feral.

        - channel hop + sniff frames
        - handshake capture + reconstruct
        - PMKID capture (when AP coughs it up)
        - optional deauth/disassoc attacks (config)
        - targeted client deauth when clients are discovered
        - hashcat 22000 exports (handshake + PMKID)

    LOCKING state shows the target SSID (up to 18 chars) and the number
    of discovered clients. more clients = more surgical deauths.

    BOAR BROS:
        press [B] in OINK to add the selected network to /boar_bros.txt
        bros get observed, not punched.

    seamless ethics toggle:
        press [D] in OINK to flip into DO NO HAM.
        same radio. different conscience.


----[ 2.2 - DO NO HAM (passive recon)

    press [D]. pig goes zen.

        - promiscuous receive only
        - zero TX (no deauth, no probes)
        - adaptive channel timing (priority 1/6/11, but still sweeps)
        - passive PMKID + handshake catches when they happen naturally

    press [D] again to go back to OINK.
    toast says something British. you'll get it.


----[ 2.3 - SGT WARHOG (wardriving)

    press [W]. strap on GPS. roll out.

        - logs discovered APs to SD
        - exports WiGLE v1.6 CSV (WigleWifi-1.6)
        - keeps a dedup cache so you don't write the same AP forever

    WARHOG files live under /wardriving/ on SD.


----[ 2.4 - HOG ON SPECTRUM (RF view)

    press [H]. watch 2.4GHz breathe.

        - spectrum lobes per channel
        - indicators for weak security (VULN)
        - indicator when a network is a BRO (BRO)
        - CLIENT MONITOR for a selected network

    CLIENT MONITOR:
        shows clients, vendor guess, RSSI, freshness, and proximity arrows.
        it's marco polo but with packets.


----[ 2.5 - PIGGY BLUES (BLE chaos)

    press [B]. become everyone's least favorite person.
    vendor-aware BLE spam (Apple/Android/Samsung/Windows).

    don't do this in public.
    yes, that's legal advice.


----[ 2.6 - FILE TRANSFER (PORKCHOP COMMANDER)

    press [F]. the pig becomes a web file server.

        - connects to your configured WiFi network
        - serves a browser UI for SD/local file management
        - mDNS name: porkchop (try porkchop.local)

    this is how you exfil your own captures without pulling the SD.


--[ 3 - XP, ranks, buffs

    the pig levels from 1 to 50.
    every action feeds the bar.
    every tier makes the pig more capable.

    classes (5 levels each):

        L01-05  SH0AT
        L06-10  SN1FF3R
        L11-15  PR0B3R
        L16-20  PWN3R
        L21-25  H4ND5H4K3R
        L26-30  M1TM B0AR
        L31-35  R00T BR1STL3
        L36-40  PMF W4RD3N
        L41-45  MLO L3G3ND
        L46-50  B4C0NM4NC3R

    SWINE STATS shows:
        - lifetime counters (nets, captures, deauths, distance, etc)
        - active buffs/debuffs (mood + session momentum)
        - class perks unlocked by tier

    Achievements exist. some are obvious. some are rude.
    we're not spoiling them here.


--[ 4 - Cloud hookups (WiGLE / WPA-SEC)

    WiGLE:
        PORK TRACKS menu uploads your wardriving .wigle.csv files.
        import keys from SD:

            /wigle_key.txt
            format: apiname:apitoken

        key file deletes itself after import.

    WPA-SEC:
        LOOT menu shows captures and cloud status.
        upload selected captures to wpa-sec.stanev.org.
        import key from SD:

            /wpasec_key.txt
            format: 32-char hex key

        key file deletes itself after import.

    both services run over TLS.
    if you're low on heap: stop promiscuous modes first.
    the pig tries to be polite.


--[ 5 - Controls

    from IDLE:

        [O] OINK MODE
        [D] DO NO HAM
        [W] WARHOG
        [H] SPECTRUM
        [B] PIGGY BLUES
        [F] FILE TRANSFER
        [S] SWINE STATS
        [T] SETTINGS

        [1] PIG DEMANDS (session challenges overlay)
        [2] PIGSYNC device select (if you know, you know)

    menu navigation:
        [;] up, [.] down, [ENTER] select
        [`] / [BKSP] back out

    screenshot:
        [P] saves /screenshots/screenshotNNN.bmp on SD

    magic button:
        [G0] is configurable in the settings


--[ 6 - Building

    we use PlatformIO.

        $ pip install platformio
        $ pio run -e m5cardputer
        $ pio run -t upload -e m5cardputer

    if it doesn't compile: the horse is on ketamine.
    the barn is probably fine.
    probably.


--[ 7 - Legal

    educational and authorized security research ONLY.

    deauth is a capability, not a right.
    attacking networks you don't own is a crime.
    tracking clients without consent is stalking.

    we made a pig that can observe and test wireless networks.
    the pig is a tool.
    tools don't make choices.
    you do.

    don't be stupid. don't be evil.
    don't make us regret publishing this.


--[ 8 - Greetz

    evilsocket + pwnagotchi.
    the ESP32 underground.
    Phrack.
    2600.
    you, for reading past the legal bit.

    Praise the sun.

    Oink.
