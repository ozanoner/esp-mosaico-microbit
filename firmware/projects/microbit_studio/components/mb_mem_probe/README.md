# mb_mem_probe

Internal-RAM sampler. Eight lines of logic, one job: make the *peak* visible.

Snapshots lie on the Mosaico. The CO5300 sits on GPSPI, and for every queued
transaction whose source buffer misses the DMA alignment the SPI driver allocates
a temporary internal buffer, copies into it and frees it after the transfer —
so a reading taken between stages misses the very dip that stops the panel
updating. Reference: `esp-mosaico-devkit` → "Internal RAM is the scarce
resource", which measured ~77 KB of bounce at `trans_queue_depth = 10`.

## Use

```c
mb_mem_probe_start();              /* once, after the UI is up */
mb_mem_probe_stage("ui-ready");    /* prints, then resets the window */
/* ... work ... */
mb_mem_probe_stage("ble-up");      /* low= holds the minimum of that window */
```

Output is one line per stage:

```
I (1234) MEM: [MEM ble-up] SRAM free=46112 largest=20480 low=43968
```

`free` and `largest` are `MALLOC_CAP_INTERNAL` — internal only. `largest` is what
matters for a task stack or a DMA buffer: a fragmented heap fails a 16 KB request
while reporting 45 KB free. Do not read these numbers as PSRAM-inclusive; the
`MALLOC_CAP_DEFAULT` figure is ~6.8 MB and means nothing here.

## Cost

One task, 3 KB stack, priority 1, 5 ms period, no allocation and no logging in
the sampling loop — so it never changes the number it measures by more than its
own stack. The running minimum is a plain `size_t` written by the sampler and
read by whoever logs a stage; a torn read is impossible on this target and the
value is diagnostic, so there is no lock to perturb the measurement.
