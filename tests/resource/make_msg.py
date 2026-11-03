"""Writes a small Outlook .msg file for sc-mime's tests: python3 make_msg.py OUT.msg
A compound file ([MS-CFB], 512-byte sectors, a mini stream for what's under 4096 bytes) of MAPI
properties ([MS-OXMSG]): a subject, a sender, To and Cc recipients, a text body, an RTF body that
wraps HTML (LZFu-compressed, with back-references), a text attachment, an inline picture, a
5000-byte attachment (in regular sectors) and an attached message of its own."""
import struct, sys

FREE, END, FAT, NOSTREAM = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD, 0xFFFFFFFF
PREBUFFER = (b"{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript "
             b"\\fdecor MS Sans SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0\\blue0\r\n\\par "
             b"\\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx")


def lzfu(raw):
    """LZFu as Outlook writes it: literals and dictionary references (2 to 17 bytes), then the end."""
    dictionary = bytearray(4096)
    dictionary[:len(PREBUFFER)] = PREBUFFER
    write = len(PREBUFFER)
    out, at, tokens = bytearray(), 0, []

    def copy(offset, length):
        nonlocal write
        got = bytearray()
        for i in range(length):
            c = dictionary[(offset + i) % 4096]
            got.append(c)
            dictionary[write] = c
            write = (write + 1) % 4096
        return bytes(got)

    while at < len(raw):
        best, best_length = None, 1
        for offset in range(4096):
            if offset == write or dictionary[offset] != raw[at]:
                continue
            # A reference may read what it writes: a position this copy wrote holds raw's byte.
            length = 0
            while length < 17 and at + length < len(raw):
                position = (offset + length) % 4096
                written = (position - write) % 4096
                c = raw[at + written] if written < length else dictionary[position]
                if c != raw[at + length]:
                    break
                length += 1
            if length > best_length:
                best, best_length = offset, length
        if best is not None and best_length >= 2:
            copy(best, best_length)
            tokens.append(struct.pack(">H", best << 4 | (best_length - 2)))
            at += best_length
        else:
            dictionary[write] = raw[at]
            write = (write + 1) % 4096
            tokens.append(bytes([raw[at]]))
            at += 1
    tokens.append(struct.pack(">H", write << 4))  # the end: a reference to where it would write
    for start in range(0, len(tokens), 8):
        group = tokens[start:start + 8]
        control = sum(1 << i for i, t in enumerate(group) if len(t) == 2)
        out.append(control)
        for t in group:
            out += t
    return struct.pack("<IIII", len(out) + 12, len(raw), 0x75465A4C, 0) + bytes(out)


def unicode(text):
    return text.encode("utf-16-le") + b"\0\0"


def fixed(header_size, values):
    """__properties_version1.0: its header, then 16 bytes a property (tag, flags, value)."""
    out = bytearray(header_size)
    for tag, value in values:
        out += struct.pack("<IIQ", tag, 6, value)
    return bytes(out)


def filetime(unix):
    return (unix + 11644473600) * 10000000


# The tree: (name, children) for a storage, (name, bytes) for a stream.
def stream(prop, ptype, data):
    return ("__substg1.0_%04X%04X" % (prop, ptype), data)


rtf = (b"{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 \\deff0{\\fonttbl{\\f0\\fswiss Arial;}}"
       b"{\\*\\htmltag19 <html>}{\\*\\htmltag34 <body>}\\htmlrtf {\\htmlrtf0 "
       b"{\\*\\htmltag64 <p>}Hello \\'e9 world, hello world, hello world\\htmlrtf\\par\\htmlrtf0 {\\*\\htmltag72 </p>}"
       b"\\htmlrtf }\\htmlrtf0 {\\*\\htmltag42 </body>}{\\*\\htmltag27 </html>}}")
big = bytes(i % 251 for i in range(5000))
inner = ("__substg1.0_3701000D", [
    stream(0x0037, 0x001F, unicode("Inner message")),
    stream(0x1000, 0x001F, unicode("The forwarded text.")),
    stream(0x0C1A, 0x001F, unicode("Bob Jones")),
    stream(0x5D01, 0x001F, unicode("bob@example.com")),
    ("__properties_version1.0", fixed(24, [(0x00390040, filetime(1700000000))])),
])
root = ("Root Entry", [
    stream(0x0037, 0x001F, unicode("Résumé review")),
    stream(0x0C1A, 0x001F, unicode("Ann Smith")),
    stream(0x5D01, 0x001F, unicode("ann@example.com")),
    stream(0x0C1F, 0x001F, unicode("/O=EXAMPLE/OU=FIRST/CN=RECIPIENTS/CN=ANN")),
    stream(0x1000, 0x001F, unicode("Hello é world, as text.")),
    stream(0x1009, 0x0102, lzfu(rtf)),
    stream(0x1035, 0x001F, unicode("<m1@example.com>")),
    ("__properties_version1.0", fixed(32, [(0x00390040, filetime(1704067200)), (0x3FDE0003, 1252)])),
    ("__recip_version1.0_#00000000", [
        stream(0x3001, 0x001F, unicode("Carol")),
        stream(0x39FE, 0x001F, unicode("carol@example.com")),
        ("__properties_version1.0", fixed(8, [(0x0C150003, 1)])),
    ]),
    ("__recip_version1.0_#00000001", [
        stream(0x3001, 0x001F, unicode("Dave")),
        stream(0x3003, 0x001F, unicode("dave@example.com")),
        ("__properties_version1.0", fixed(8, [(0x0C150003, 2)])),
    ]),
    ("__attach_version1.0_#00000000", [
        stream(0x3707, 0x001F, unicode("notes.txt")),
        stream(0x370E, 0x001F, unicode("text/plain")),
        stream(0x3701, 0x0102, b"Some notes.\n"),
        ("__properties_version1.0", fixed(8, [(0x37050003, 1)])),
    ]),
    ("__attach_version1.0_#00000001", [
        stream(0x3707, 0x001F, unicode("logo.png")),
        stream(0x370E, 0x001F, unicode("image/png")),
        stream(0x3712, 0x001F, unicode("logo@example.com")),
        stream(0x3701, 0x0102, b"\x89PNG\r\n\x1a\nnot really a picture"),
        ("__properties_version1.0", fixed(8, [(0x37050003, 1), (0x37140003, 4)])),
    ]),
    ("__attach_version1.0_#00000002", [
        stream(0x3707, 0x001F, unicode("big.bin")),
        stream(0x3701, 0x0102, big),
        ("__properties_version1.0", fixed(8, [(0x37050003, 1)])),
    ]),
    ("__attach_version1.0_#00000003", [
        stream(0x3001, 0x001F, unicode("Inner message")),
        inner,
        ("__properties_version1.0", fixed(8, [(0x37050003, 5)])),
    ]),
])

# Directory entries in order; each storage's children chained through their right siblings.
entries = []


def add(node):
    index = len(entries)
    name, content = node
    entries.append({"name": name, "storage": isinstance(content, list), "data": None if isinstance(content, list) else content,
                    "child": NOSTREAM, "right": NOSTREAM})
    if isinstance(content, list):
        children = [add(child) for child in content]
        if children:
            entries[index]["child"] = children[0]
            for a, b in zip(children, children[1:]):
                entries[a]["right"] = b
    return index


add(root)
entries[0]["storage"] = "root"

# Small streams in the mini stream (64-byte sectors), big ones in sectors of their own.
mini, mini_fat = bytearray(), []
for e in entries:
    if e["data"] is not None and len(e["data"]) < 4096:
        e["start"] = len(mini) // 64
        sectors = max(1, (len(e["data"]) + 63) // 64)
        mini_fat += [e["start"] + i + 1 for i in range(sectors - 1)] + [END]
        mini += e["data"] + b"\0" * (sectors * 64 - len(e["data"]))
if not mini_fat:
    mini_fat = [END]

directory = bytearray()
sectors = []  # (name, bytes) in file order, each a whole number of 512-byte sectors


def place(data):
    padded = data + b"\0" * ((-len(data)) % 512)
    count = len(padded) // 512
    return padded, count


layout = []
directory_size = len(entries) * 128
blocks = {"directory": bytes(directory_size), "minifat": b"".join(struct.pack("<I", x) for x in mini_fat), "ministream": bytes(mini)}
for e in entries:
    if e["data"] is not None and len(e["data"]) >= 4096:
        blocks["big%d" % id(e)] = e["data"]
order = ["directory", "minifat", "ministream"] + [k for k in blocks if k.startswith("big")]
counts = {k: place(blocks[k])[1] for k in order}
data_sectors = sum(counts.values())
fat_sectors = 1
while fat_sectors * 128 < data_sectors + fat_sectors:
    fat_sectors += 1
start = {}
position = fat_sectors
for k in order:
    start[k] = position
    position += counts[k]
fat = [FAT] * fat_sectors
for k in order:
    fat += [start[k] + i + 1 for i in range(counts[k] - 1)] + [END]
fat += [FREE] * (fat_sectors * 128 - len(fat))
for e in entries:
    if e["data"] is not None and len(e["data"]) >= 4096:
        e["start"] = start["big%d" % id(e)]

for i, e in enumerate(entries):
    name = e["name"].encode("utf-16-le") + b"\0\0"
    kind = 5 if e["storage"] == "root" else 1 if e["storage"] else 2
    if kind == 5:
        first, size = start["ministream"], len(mini)
    elif kind == 1:
        first, size = 0, 0
    else:
        first, size = e["start"], len(e["data"])
    directory += struct.pack("<64sHBBIII16sIQQIQ", name, len(name), kind, 1, NOSTREAM, e["right"], e["child"], b"\0" * 16, 0, 0, 0, first, size)
blocks["directory"] = bytes(directory)

header = bytearray(512)
header[0:8] = bytes([0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1])
struct.pack_into("<HHHHH", header, 0x18, 0x3E, 3, 0xFFFE, 9, 6)
struct.pack_into("<IIIIIIIII", header, 0x28, 0, fat_sectors, start["directory"], 0, 4096, start["minifat"], counts["minifat"], END, 0)
difat = list(range(fat_sectors)) + [FREE] * (109 - fat_sectors)
struct.pack_into("<109I", header, 0x4C, *difat)

with open(sys.argv[1], "wb") as out:
    out.write(header)
    out.write(b"".join(struct.pack("<I", x) for x in fat))
    for k in order:
        out.write(place(blocks[k])[0])
