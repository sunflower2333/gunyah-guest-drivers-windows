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

For on-device final-screen color sampling, use `--frames 6 --color-hold-ms 2000`.
This holds distinctive RGB colors and prints the expected values and Guest tick
for each frame. The existing workspace `chat/tools/native-solid-color.c` helper
can inspect SurfaceFlinger pixels entirely on Android and return only numeric
counts; no screenshot needs to be pulled. Compare a before-test baseline, the
active color interval and the observed window location. This is a functional
pixel check, not FPS or zero-copy proof. The same host identities and copy/fence
evidence remain required.
