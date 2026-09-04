// POMIIGS — Apple IIgs emulator
// VERHILLE Arnaud — Copyright (C) 2026 — GPLv3 (see LICENSE)
//
// WOZ2 3.5" media on the Sony LLE. A patterned 800K sector image exported as
// a WOZ2 (saveAsWoz: nibblised tracks, $FF as 10-bit syncs) must decode back
// to the identical image through loadWoz (IWM-style bit assembly + the KEGS
// denibbliser); the container must be well-formed; a WOZ1 or 5.25" WOZ is
// refused. The independent-decoder check (cp2 reading what the ROM formatted)
// lives in the MiSTer gate (sony_format_woz).

#include "Sony35.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    int fails = 0;
    auto expect = [&](const char* what, bool ok) { if (!ok) { std::printf("FAIL %s\n", what); ++fails; } };
    auto readFile = [](const std::string& p) { std::ifstream in(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); };
    auto writeFile = [](const std::string& p, const std::vector<uint8_t>& d) { std::ofstream o(p, std::ios::binary); o.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size())); };

    std::vector<uint8_t> image(819200);
    for (size_t i = 0; i < image.size(); ++i) image[i] = uint8_t((i >> 9) * 7 + (i & 0xFF) * 3 + 1);
    const std::string po = dir + "/sony_woz_test.po", woz = dir + "/sony_woz_test.woz";
    writeFile(po, image);

    Sony35 a;
    expect("load the .po", a.loadImage(po));
    expect("export as WOZ2", a.saveAsWoz(woz));
    const std::vector<uint8_t> file = readFile(woz);
    expect("WOZ2 magic", file.size() > 12 && file[0] == 'W' && file[1] == 'O' && file[2] == 'Z' && file[3] == '2');
    expect("INFO chunk first", file.size() > 20 && std::string(file.begin() + 12, file.begin() + 16) == "INFO");
    expect("INFO: 3.5\" disk, 2 sides", file[21] == 2 && file[20 + 37] == 2);
    expect("TMAP identity", file.size() > 88 + 160 && file[88] == 0 && file[88 + 159] == 159);

    Sony35 b;
    expect("load the WOZ2", b.loadImage(woz));
    expect("decoded image identical to the source", b.loaded() && b.image() == image);
    expect("WOZ media is writable (INFO wp = 0)", !b.writeProtected());

    // Refusals: WOZ1, and a WOZ2 that is not a 3.5" disk.
    std::vector<uint8_t> woz1 = file; woz1[3] = '1'; writeFile(dir + "/sony_woz_test1.woz", woz1);
    Sony35 c; expect("WOZ1 refused", !c.loadImage(dir + "/sony_woz_test1.woz"));
    std::vector<uint8_t> w525 = file; w525[21] = 1; writeFile(dir + "/sony_woz_test525.woz", w525);
    Sony35 d; expect("5.25\" WOZ refused", !d.loadImage(dir + "/sony_woz_test525.woz"));

    // Optional: an externally-authored WOZ (cp2) must at least LOAD — its raw
    // nibbles are served to the guest even when our sector-decoder rejects the
    // tool's specific nibble layout (informational; the guest-level round trip
    // is proven by the floppy_rw35_woz / sony_format_woz gates).
    if (argc > 2) {
        Sony35 e;
        expect("load the external WOZ", e.loadImage(argv[2]));
    }
    for (const char* f : {"/sony_woz_test.po", "/sony_woz_test.woz", "/sony_woz_test1.woz", "/sony_woz_test525.woz"}) std::remove((dir + f).c_str());
    if (fails) { std::printf("sony_woz_test: %d failure(s)\n", fails); return 1; }
    std::printf("OK: WOZ2 3.5\" export/decode round trip, container layout, refusals\n");
    return 0;
}
