# Native AHB functional probe

Run the ARM64 executable in the interactive Windows desktop after installing
the matched KMD, Mesa and Android host. It selects only the hardware VIOGPU
PCI adapter and never uses WARP. `--native` opts this process into the native
AHB UMD path before creating its device.

```
native_display_probe.exe --native --resize --frames 240 --idle-ms 6000
```

The test uses three flip-sequential BGRA buffers, alternating GPU clears,
two resizes (including a padded-stride 513x257 extent), and an idle front
buffer interval longer than the host's old five-second release timeout.
There is no pixel readback and no deliberate intermediate pixel copy in
this test. A separate 60-second process watchdog bounds driver stalls.

A success proves only that the hardware API operations completed without
device removal. Require matching host/KMD allocation identities, actual
native accepted/released counters, zero presentation copy/blit counters,
and visibly correct frames before reporting full-chain zero-copy. The
probe explicitly reports `zero_copy_verified=0`; an environment flag and
successful `Present` calls alone cannot establish the display transport.
