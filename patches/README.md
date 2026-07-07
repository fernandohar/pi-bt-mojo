# ESP-IDF v6 diagnostic patches

These patches apply to **your local ESP-IDF v6 checkout** (not to this firmware repo).
They add extra logging inside Bluedroid to pin down why the v6 external-codec
A2DP **sink** rejects the iPhone during capability negotiation.

## 0001-mojo-diag-peer-src-supports-codec.patch

Adds `APPL_TRACE_ERROR("MOJO-DIAG ...")` lines inside
`bta_av_co_audio_peer_src_supports_codec()` in
`components/bt/host/bluedroid/btc/profile/std/a2dp/bta_av_co.c`.

It prints, on every connection attempt:

- `codec_cfg.id`   – the codec id our sink is currently trying to match
- `caps[cur].id` / `caps.info[0..2]` – how **our** registered SEP is stored
  (`info[0]` is the SBC "losc"/length byte that `A2D_ParsSbcInfo` requires to be `6`)
- `src[i].codec_type` / `srcs.caps[0..2]` – what the **iPhone** advertised
- `num_sup_srcs`

These lines print at **ERROR** level, so they show even without DEBUG logging.

### Apply

```sh
cd $IDF_PATH            # your esp-idf v6 checkout, e.g. ~/esp/esp-idf-v6
git apply /path/to/pi-bt-mojo/patches/0001-mojo-diag-peer-src-supports-codec.patch
```

Then do a **clean** rebuild of the firmware so the stack is recompiled:

```sh
cd pi-bt-mojo
idf.py fullclean
idf.py build flash monitor
```

Reproduce the connection attempt and capture the `MOJO-DIAG` lines.

### Revert

```sh
cd $IDF_PATH
git checkout -- components/bt/host/bluedroid/btc/profile/std/a2dp/bta_av_co.c
```
