# mb_payload

C port of `tools/hexkit`: the Intel HEX geometry the flasher needs, where the
DAL magic sits, which eight bytes are the template and program hashes, where
region 2 starts and ends, and how it packs into 64-byte blocks.

`tools/hexkit/hexkit.py` stays the reference implementation. Where the two
disagree, hexkit is right and this file is the bug.

## Why a port at all

The Mosaico has to make the same decisions the build service made when it
produced the image: is this hex a MakeCode image, which region may be written,
does the DAL match. Doing that in C keeps the device independent of the build
service's language, and gives the host test something to compare against.

## Differences from hexkit, deliberate

- **Only region 2 is kept.** hexkit builds a sparse map of the whole image; this
  port stores a fixed window, `0x30000-0x3BBFF` (47.6 KB), supplied by the
  caller. The device never parses a 1.3 MB hex: it streams the pre-extracted
  fixture, so the window exists for the host test and for future on-device use.
- **The DAL magic is searched inside that window.** If a future image carried the
  magic outside region 2, hexkit would report a range error and this port would
  report `MB_PAYLOAD_ERR_NO_MAGIC`. Same refusal, different code.
- **Contiguity is checked at 64-byte granularity** between the magic and the
  covered end, which is the granularity the transfer writes at.

## Testing

Two levels, both runnable on a host:

```sh
cc -std=c11 -Wall -Wextra -Werror -Iinclude host/test_payload.c mb_payload.c -o /tmp/t
python3 host/compare.py ../../../../build/hello.hex /tmp/t
```

`compare.py` recomputes every value with hexkit and prints the first difference;
the FNV-1a digest covers all block bytes, so "matches" means byte-for-byte, not
"same field names".

Expected on the pinned toolchain (hello build):

```
magic 0x37400   template 08d9ef69355ad697   program 8f007d004500b100
region2 0x37400-0x38BFF   blocks 96   fnv1a64 6118c6f16c41e66b
```

`MB_PAYLOAD_WINDOW_BYTES` is the buffer size a caller must provide; there is no
allocation inside the port.
