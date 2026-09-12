# Display timing boundary regression

`run-msvc.cmd` compiles the production `common/display_timing.h` parser under
MSVC `/W4 /WX` in the ARM64 WDK workflow. The host executable is a parser test;
it is not evidence of Windows monitor enumeration or rendered frame rate.

`host_edid_fixtures.h` contains unmodified bytes emitted by crosvm commit
`6a4bd43184a86b49105ef1bc2e77c11ab9ee744a` from the actual production module:

```sh
rustc --edition=2021 devices/tests/edid_contract.rs -o edid-generator
./edid-generator 3040 1904 165 3040x1904-165.bin
./edid-generator 1920 1080 165 1920x1080-165.bin
./edid-generator 7680 4320 240 7680x4320-240.bin
xxd -i -n host3040 3040x1904-165.bin
xxd -i -n host1920 1920x1080-165.bin
xxd -i -n host7680 7680x4320-240.bin
```

The committed arrays are immutable fixtures. The test can additionally consume
the three fresh binaries in that order. Both paths require the exact byte
count, preferred mode, totals, 64-bit pixel clock, reduced refresh rational and
timer period. A 128-byte legacy fixture reproducing the installed
3040x1904@71.832679Hz wrap must fail the 165Hz acceptance condition.

The tests also cover malformed and truncated sections, independent inner and
outer checksums, CTA/DisplayID in either extension order, capacity boundaries,
and pixel clocks beyond 32 bits. ASan/UBSan invocation on Linux:

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  viogpu/tests/display-timing/display_timing_test.cpp -o timing-tests
./timing-tests 3040x1904-165.bin 1920x1080-165.bin 7680x4320-240.bin
```

Independent `edid-decode` from v4l-utils mirrored at a1ive/edid-decode
`68eda65b45dc38a15cc86307e302d3b0346aebc5` reports conformity PASS for all three
fixtures; its only warning is the inherited dummy serial number 1. Packaged
edid-decode `cb74358c2896` also reports PASS for the target 3040x1904 fixture,
with an older PNP/OUI warning for the inherited GGL vendor identity. The old
vendored libdisplay-info parser correctly decodes the timing but reports an
inherited unset white point and a parser padding defect; it is not reported as
a conformity pass.
