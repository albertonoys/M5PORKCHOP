#!/usr/bin/env python3
"""
WPA-SEC PCAP Compliance Checker
================================
because apparently we need to verify our own shit works.

validates pcap files against wpa-sec.stanev.org requirements:
- pcap magic number (little or big endian, we're not picky)
- LINKTYPE_IEEE802_11_RADIOTAP (127, the chosen one)
- radiotap header (8 bytes minimum, we're not animals)
- beacon frame (for ESSID extraction, or wpa-sec cries)
- EAPOL M1/M2/M3/M4 frames (the four horsemen of handshake)

usage: python wpasec_check.py <file_or_directory>

no dependencies. pure python. like god intended.
we parse bytes like it's 1999.

"""

import os
import sys
import struct
from pathlib import Path


# =============================================================================
# CONSTANTS - because magic numbers deserve names
# =============================================================================

# pcap global header magic numbers
# one of these better be at offset 0 or we're dealing with a potato, not a pcap
PCAP_MAGIC_LE = b'\xd4\xc3\xb2\xa1'  # little-endian, the sane default
PCAP_MAGIC_BE = b'\xa1\xb2\xc3\xd4'  # big-endian, for people who hate themselves
PCAPNG_MAGIC  = b'\x0a\x0d\x0d\x0a'  # pcapng, fancy pants format

# link types - we only care about one
# if you captured with something else, that's a you problem
LINKTYPE_IEEE802_11_RADIOTAP = 127

# 802.11 frame types - the taxonomy of wifi packets
# frame control byte: (subtype << 4) | (type << 2) | version
# type 0 = management, subtype 8 = beacon
# yes, wifi frame format was designed by a committee. you can tell.
FRAME_TYPE_MGMT = 0
FRAME_SUBTYPE_BEACON = 8

# EAPOL - the authentication dance
# ethertype 0x888E because 0x8086 was taken and 0xDEAD was too obvious
EAPOL_ETHERTYPE = b'\x88\x8e'
EAPOL_TYPE_KEY = 0x03  # we want keys, not identity or notifications

# Key Information bit positions (802.11i, section 8.5.2)
# whoever designed this bitfield was either drunk or a genius. possibly both.
KEYINFO_BIT_INSTALL = 6   # bit 6: Install (M3 sets this, everyone else: nah)
KEYINFO_BIT_ACK     = 7   # bit 7: Key Ack (AP says "got it", STA says nothing)
KEYINFO_BIT_MIC     = 8   # bit 8: Key MIC (proof you're not making shit up)
KEYINFO_BIT_SECURE  = 9   # bit 9: Secure (encrypted, for real this time)


# =============================================================================
# CLASSES - because even hackers appreciate structure
# =============================================================================

class PcapAnalysis:
    """
    Results container. 
    Like a medical report, but for your pcap's health.
    Spoiler: it's probably fine. probably.
    """
    def __init__(self, filename):
        self.filename = filename
        self.filesize = 0
        self.magic_ok = False
        self.magic_type = None
        self.linktype = None
        self.linktype_ok = False
        self.radiotap_ok = False
        self.radiotap_len = 0
        self.has_beacon = False
        self.beacon_ssid = None
        self.eapol_frames = []  # list of M1, M2, M3, M4
        self.packet_count = 0
        self.errors = []
        
    @property
    def has_m1(self):
        return 'M1' in self.eapol_frames
    
    @property
    def has_m2(self):
        return 'M2' in self.eapol_frames
    
    @property
    def has_m3(self):
        return 'M3' in self.eapol_frames
    
    @property
    def has_m4(self):
        return 'M4' in self.eapol_frames
    
    @property
    def full_handshake(self):
        """all four horsemen present and accounted for"""
        return self.has_m1 and self.has_m2 and self.has_m3 and self.has_m4
    
    @property
    def wpasec_compatible(self):
        """
        wpa-sec minimum requirements:
        1. valid pcap with radiotap
        2. beacon frame (they need the ESSID, obviously)
        3. at least M2 or M4 (the ones with the goodies)
        
        without beacon, hcxpcapngtool will cry and refuse to cooperate.
        we've all been there, hcxpcapngtool. we've all been there.
        """
        return (self.magic_ok and 
                self.linktype_ok and 
                self.radiotap_ok and 
                self.has_beacon and 
                (self.has_m2 or self.has_m4))
    
    @property
    def hashcat_ready(self):
        """
        hashcat 22000 needs:
        - beacon (ESSID)
        - M2 (has SNonce) or M4 (has everything)
        
        technically M1+M2 is the minimum for a challenge.
        M2+M3 or M3+M4 also work. it's complicated.
        like my relationship with 802.11.
        """
        return self.has_beacon and (self.has_m2 or self.has_m4)


# =============================================================================
# PARSING FUNCTIONS - where the real magic happens
# =============================================================================

def read_uint16_le(data, offset):
    """
    read 2 bytes, little-endian.
    because intel won and we all have to live with it.
    """
    return struct.unpack('<H', data[offset:offset+2])[0]


def read_uint32_le(data, offset):
    """
    read 4 bytes, little-endian.
    the only correct byte order. fight me.
    """
    return struct.unpack('<I', data[offset:offset+4])[0]


def read_uint16_be(data, offset):
    """
    read 2 bytes, big-endian.
    for network byte order, aka "the old way" aka "the confusing way"
    """
    return struct.unpack('>H', data[offset:offset+2])[0]


def classify_eapol_frame(key_info):
    """
    determine which message of the 4-way handshake this is.
    
    the 4-way handshake goes:
    M1: AP -> STA: "here's my ANonce, prove you know the password"
    M2: STA -> AP: "here's my SNonce + MIC, i know the password"  
    M3: AP -> STA: "cool, here's the GTK, install the keys"
    M4: STA -> AP: "keys installed, we're good"
    
    Key Information bits tell us which message:
    M1: Ack=1, MIC=0 (AP sends, no proof yet)
    M2: Ack=0, MIC=1, Secure=0 (STA responds, not secure yet)
    M3: Ack=1, MIC=1, Install=1 (AP confirms, install keys)
    M4: Ack=0, MIC=1, Secure=1 (STA confirms, all secure)
    
    if this seems overcomplicated, that's because it is.
    thanks, IEEE. really appreciate the job security.
    """
    ack = (key_info >> KEYINFO_BIT_ACK) & 1
    mic = (key_info >> KEYINFO_BIT_MIC) & 1
    secure = (key_info >> KEYINFO_BIT_SECURE) & 1
    install = (key_info >> KEYINFO_BIT_INSTALL) & 1
    
    if ack and not mic:
        return 'M1'
    elif not ack and mic and not secure:
        return 'M2'
    elif ack and mic and install:
        return 'M3'
    elif not ack and mic and secure:
        return 'M4'
    else:
        # some weird EAPOL that doesn't fit the pattern
        # group key handshake? rekeying? who knows.
        return None


def extract_ssid_from_beacon(packet_data, offset):
    """
    extract SSID from beacon frame.
    
    beacon frame structure (after 802.11 header):
    - timestamp: 8 bytes
    - beacon interval: 2 bytes
    - capability info: 2 bytes
    - tagged parameters: variable (this is where SSID lives)
    
    SSID tag: type=0, length=N, data=SSID
    
    returns None if SSID not found or hidden (length 0).
    hidden SSIDs are a privacy theater. we all know this.
    """
    # 802.11 management header is typically 24 bytes
    # then fixed fields: timestamp(8) + interval(2) + capability(2) = 12 bytes
    # tags start at offset + 24 + 12 = offset + 36
    
    try:
        tag_start = offset + 36
        
        # parse tagged parameters
        while tag_start < len(packet_data) - 2:
            tag_type = packet_data[tag_start]
            tag_len = packet_data[tag_start + 1]
            
            if tag_type == 0:  # SSID tag
                if tag_len > 0 and tag_start + 2 + tag_len <= len(packet_data):
                    ssid = packet_data[tag_start + 2:tag_start + 2 + tag_len]
                    try:
                        return ssid.decode('utf-8', errors='replace')
                    except:
                        return ssid.hex()
                return None  # hidden SSID (length 0)
            
            tag_start += 2 + tag_len
            
            # sanity check: don't loop forever on malformed data
            if tag_len == 0 and tag_type != 0:
                break
                
    except (IndexError, struct.error):
        pass
    
    return None


def analyze_pcap(filepath):
    """
    the main event. parse a pcap file and judge it mercilessly.
    
    pcap format (libpcap):
    - global header: 24 bytes
      - magic: 4 bytes (tells us endianness)
      - version: 4 bytes (we don't care)
      - timezone: 8 bytes (we definitely don't care)
      - snaplen: 4 bytes (max packet size)
      - linktype: 4 bytes (we care about this a lot)
    
    - packet headers: 16 bytes each
      - timestamp: 8 bytes
      - captured length: 4 bytes
      - original length: 4 bytes
    
    - packet data: variable
      - radiotap header (if linktype 127)
      - 802.11 frame
      - whatever was in the air
    
    we're basically doing what wireshark does, but worse.
    much worse. but it works. probably.
    """
    result = PcapAnalysis(os.path.basename(filepath))
    
    try:
        with open(filepath, 'rb') as f:
            data = f.read()
    except Exception as e:
        result.errors.append(f"can't read file: {e}")
        return result
    
    result.filesize = len(data)
    
    if len(data) < 24:
        result.errors.append("file too small to be a pcap (less than 24 bytes)")
        return result
    
    # ==========================================================================
    # CHECK MAGIC NUMBER
    # ==========================================================================
    magic = data[0:4]
    
    if magic == PCAP_MAGIC_LE:
        result.magic_ok = True
        result.magic_type = "pcap (little-endian)"
    elif magic == PCAP_MAGIC_BE:
        result.magic_ok = True
        result.magic_type = "pcap (big-endian)"
        result.errors.append("big-endian pcap detected. you're a psychopath, but we support you.")
    elif magic == PCAPNG_MAGIC:
        result.magic_ok = True
        result.magic_type = "pcapng"
        result.errors.append("pcapng detected. fancy. we support it, but parsing is TODO.")
        # pcapng is a whole different beast. for now, just acknowledge it.
        return result
    else:
        result.errors.append(f"unknown magic: {magic.hex()}. this is not a pcap.")
        return result
    
    # ==========================================================================
    # CHECK LINKTYPE
    # ==========================================================================
    result.linktype = read_uint32_le(data, 20)
    
    if result.linktype == LINKTYPE_IEEE802_11_RADIOTAP:
        result.linktype_ok = True
    else:
        result.errors.append(
            f"linktype {result.linktype} is not radiotap (127). "
            f"wpa-sec will be disappointed in you."
        )
        return result
    
    # ==========================================================================
    # PARSE PACKETS
    # ==========================================================================
    offset = 24  # skip global header
    first_packet = True
    
    while offset + 16 <= len(data):
        # packet header
        # ts_sec = read_uint32_le(data, offset)  # don't care
        # ts_usec = read_uint32_le(data, offset + 4)  # really don't care
        incl_len = read_uint32_le(data, offset + 8)
        # orig_len = read_uint32_le(data, offset + 12)  # meh
        
        packet_start = offset + 16
        packet_end = packet_start + incl_len
        
        if packet_end > len(data):
            result.errors.append(f"packet extends beyond file at offset {offset}. truncated capture?")
            break
        
        result.packet_count += 1
        
        # ======================================================================
        # CHECK RADIOTAP HEADER (first packet only for efficiency)
        # ======================================================================
        if first_packet and incl_len >= 4:
            radiotap_version = data[packet_start]
            # radiotap_pad = data[packet_start + 1]  # unused
            radiotap_len = read_uint16_le(data, packet_start + 2)
            
            result.radiotap_len = radiotap_len
            
            if radiotap_version == 0 and radiotap_len >= 8:
                result.radiotap_ok = True
            else:
                result.errors.append(
                    f"radiotap header weird: version={radiotap_version}, len={radiotap_len}. "
                    f"expected version 0, len >= 8."
                )
            first_packet = False
        
        # ======================================================================
        # PARSE 802.11 FRAME
        # ======================================================================
        # radiotap header length (assume 8 if we haven't read it yet)
        rt_len = result.radiotap_len if result.radiotap_len else 8
        frame_start = packet_start + rt_len
        
        if frame_start + 2 > packet_end:
            offset = packet_end
            continue
        
        # frame control (first 2 bytes of 802.11 frame)
        frame_control = data[frame_start]
        frame_type = (frame_control >> 2) & 0x03
        frame_subtype = (frame_control >> 4) & 0x0F
        
        # ----------------------------------------------------------------------
        # CHECK FOR BEACON FRAME
        # ----------------------------------------------------------------------
        if frame_type == FRAME_TYPE_MGMT and frame_subtype == FRAME_SUBTYPE_BEACON:
            result.has_beacon = True
            ssid = extract_ssid_from_beacon(data, frame_start)
            if ssid and not result.beacon_ssid:
                result.beacon_ssid = ssid
        
        # ----------------------------------------------------------------------
        # CHECK FOR EAPOL FRAME
        # ----------------------------------------------------------------------
        # EAPOL is in data frames (type 2), encapsulated in LLC/SNAP
        # but let's just search for the 0x888E ethertype anywhere in the packet
        # because 802.11 header length varies based on flags and we're lazy
        
        eapol_pos = data.find(EAPOL_ETHERTYPE, frame_start, packet_end)
        
        if eapol_pos != -1:
            # found EAPOL ethertype!
            # EAPOL structure after ethertype:
            # - version: 1 byte (we don't care)
            # - type: 1 byte (0x03 = Key)
            # - length: 2 bytes
            # - descriptor type: 1 byte
            # - key info: 2 bytes (this is the money shot)
            
            if eapol_pos + 9 <= packet_end:  # need at least 9 bytes after ethertype
                eapol_type = data[eapol_pos + 3]
                
                if eapol_type == EAPOL_TYPE_KEY:
                    # key info is at offset +7, +8 from ethertype (big-endian)
                    key_info = read_uint16_be(data, eapol_pos + 7)
                    msg_type = classify_eapol_frame(key_info)
                    
                    if msg_type:
                        result.eapol_frames.append(msg_type)
        
        offset = packet_end
    
    return result


# =============================================================================
# OUTPUT - making the results readable
# =============================================================================

def print_result(result, verbose=False):
    """
    print analysis results in a format that won't make your eyes bleed.
    well, maybe a little.
    """
    # status emoji because we're professionals
    status = "✅" if result.wpasec_compatible else "❌"
    
    # frame list
    unique_frames = list(dict.fromkeys(result.eapol_frames))  # preserve order, remove dupes
    frames_str = ' '.join(unique_frames) if unique_frames else 'none'
    
    # build status flags
    flags = []
    if result.magic_ok:
        flags.append("PCAP:OK")
    else:
        flags.append("PCAP:BAD")
    
    if result.linktype_ok:
        flags.append("RT:127")
    
    if result.radiotap_ok:
        flags.append(f"HDR:{result.radiotap_len}B")
    
    if result.has_beacon:
        flags.append("BCN")
    
    # handshake status
    if result.full_handshake:
        hs_status = "[FULL-4WAY]"
    elif result.hashcat_ready:
        hs_status = "[HASHCAT]"
    elif result.eapol_frames:
        hs_status = "[PARTIAL]"
    else:
        hs_status = "[NO-EAPOL]"
    
    print(f"{status} {result.filename:<24} {hs_status:<12} {' '.join(flags):<20} | {frames_str}")
    
    if verbose:
        if result.beacon_ssid:
            print(f"   SSID: {result.beacon_ssid}")
        if result.errors:
            for err in result.errors:
                print(f"   ⚠️  {err}")
        print(f"   Packets: {result.packet_count}, Size: {result.filesize} bytes")


def print_summary(results):
    """
    aggregate stats because management loves numbers.
    """
    total = len(results)
    wpasec_ok = sum(1 for r in results if r.wpasec_compatible)
    full_hs = sum(1 for r in results if r.full_handshake)
    has_beacon = sum(1 for r in results if r.has_beacon)
    
    print("\n" + "="*70)
    print("SUMMARY")
    print("="*70)
    print(f"Total files:           {total}")
    print(f"WPA-SEC compatible:    {wpasec_ok}/{total} ({100*wpasec_ok//total if total else 0}%)")
    print(f"Full 4-way handshake:  {full_hs}/{total}")
    print(f"Has beacon frame:      {has_beacon}/{total}")
    
    if wpasec_ok < total:
        print("\n⚠️  Some files may fail WPA-SEC upload:")
        for r in results:
            if not r.wpasec_compatible:
                reasons = []
                if not r.has_beacon:
                    reasons.append("missing beacon")
                if not r.has_m2 and not r.has_m4:
                    reasons.append("no M2/M4 frames")
                if not r.magic_ok:
                    reasons.append("invalid pcap")
                if not r.linktype_ok:
                    reasons.append("wrong linktype")
                print(f"   - {r.filename}: {', '.join(reasons)}")


# =============================================================================
# MAIN - where it all comes together
# =============================================================================

def main():
    """
    entry point.
    give us files, we judge them.
    it's a simple arrangement.
    """
    if len(sys.argv) < 2:
        print(__doc__)
        print("\nusage: python wpasec_check.py <file_or_directory> [--verbose]")
        print("\nexamples:")
        print("  python wpasec_check.py capture.pcap")
        print("  python wpasec_check.py /path/to/captures/")
        print("  python wpasec_check.py *.pcap --verbose")
        sys.exit(1)
    
    verbose = '--verbose' in sys.argv or '-v' in sys.argv
    paths = [p for p in sys.argv[1:] if not p.startswith('-')]
    
    files_to_check = []
    
    for path in paths:
        p = Path(path)
        if p.is_file():
            files_to_check.append(str(p))
        elif p.is_dir():
            # find all pcap files in directory
            for ext in ['*.pcap', '*.cap', '*.pcapng']:
                files_to_check.extend(str(f) for f in p.glob(ext))
        else:
            print(f"⚠️  path not found: {path}")
    
    if not files_to_check:
        print("no pcap files found. nothing to do. pig is disappointed.")
        sys.exit(1)
    
    print("="*70)
    print("WPA-SEC PCAP COMPLIANCE CHECK")
    print("="*70)
    print(f"{'STATUS':<3} {'FILENAME':<24} {'HANDSHAKE':<12} {'FLAGS':<20} | FRAMES")
    print("-"*70)
    
    results = []
    for filepath in sorted(files_to_check):
        result = analyze_pcap(filepath)
        results.append(result)
        print_result(result, verbose)
    
    if len(results) > 1:
        print_summary(results)
    
    # exit code: 0 if all files are wpa-sec compatible
    sys.exit(0 if all(r.wpasec_compatible for r in results) else 1)


if __name__ == '__main__':
    main()
