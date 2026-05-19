# otm-bridge

OpenWrt package that turns a Linux monitor interface into an
[opentrafficmap.org](https://opentrafficmap.org/) C-ITS receiver.

It captures raw 802.11(p) frames (with radiotap headers) on a configurable
monitor interface and publishes each frame as-is to OTM's public MQTT ingest,
matching the wire format of the
[ESP32-C5 reference firmware](https://codeberg.org/opentrafficmap/its-g5-receiver-firmware).

## Wire format (mimics the ESP32-C5 firmware)

Broker: `mqtts://cits1.opentrafficmap.org:8883` — TLS, server-cert only, no
client auth, no registration required.

Topics (with default node-id = `phy0` MAC, lowercase 12 hex chars, no
separators):

| Topic | Payload | When |
| --- | --- | --- |
| `its/<node>/status` | `online` / `offline` (LWT, retained) | on connect / disconnect |
| `its/<node>/info`   | `{"emac":"aa:bb:..","ver":"otm-bridge-<v>","hwv":"FRITZ!Box 3390"}` | on connect |
| `its/<node>/packet` | raw 802.11 frame (radiotap-prefixed bytes) | per captured frame, QoS 0 |

## What's in the box

The package ships:

* `/usr/bin/otm-bridge`         — the C daemon (libpcap + libmosquitto-ssl)
* `/etc/config/otm-bridge`      — UCI config (interface, broker, node-id, capture options)
* `/etc/init.d/otm-bridge`      — procd init with two instances:
    * **`sniffer`**: rotating `tcpdump` to `/tmp/mon0-YYYYMMDD-HHMMSS.pcapN`
      (default 5 × 1 MB ring, hourly rotation) — capture starts immediately
      after `mon0` is up so no frames are lost during NTP sync
    * **`bridge`**: the MQTT publisher, gated by NTP sync (`/var/state/dnsmasqsec`
      marker + 15 s slack so the first TLS handshake doesn't trip on an
      in-transit clock-step)
* `/etc/uci-defaults/99-otm-setup` — runs once on first boot to configure:
    * `lan1` as WAN (DHCP + IPv6 RA)
    * SSH allowed from the WAN zone
    * hostname `opentrafficmap`

The init script also flips the WLAN LED trigger from `phy0tpt` (throughput)
to `phy0rx` (per-frame), so the LED visibly blinks on every received frame.

## Using this as an OpenWrt feed

```
echo "src-git otm-bridge https://github.com/MPW1412/openwrt-otm-bridge.git" \
    >> openwrt/feeds.conf.default
cd openwrt
./scripts/feeds update otm-bridge
./scripts/feeds install -a -p otm-bridge
# enable in menuconfig: Utilities → otm-bridge
```

For the full image-build orchestrator that pre-applies the necessary
mac80211 / ath9k / wireless-regdb V2X patches needed for 5,9 GHz operation,
see [`avm-fritz3390-802.11p-otm`](https://github.com/MPW1412/avm-fritz3390-802.11p-otm).

## UCI reference (`/etc/config/otm-bridge`)

```
config otm-bridge 'main'
    option enabled         '1'
    option iface           'mon0'                                  # monitor interface
    option phy             'phy0'                                  # physical radio
    option freq            '5890'                                  # ITS-G5 CCH
    option bw              '10MHz'
    option broker          'mqtts://cits1.opentrafficmap.org'
    option node            ''                                      # empty -> phy0 MAC
    option verbose         '1'
    option capture         '1'
    option capture_dir     '/tmp'
    option capture_size    '1'                                     # MB per file
    option capture_files   '5'                                     # ring length
```

## Hardware requirements

Anything with an `ath9k`-driven radio that can be put into monitor mode on
5,9 GHz, and a kernel that knows about the DSRC channels. The companion repo
[`avm-fritz3390-802.11p-otm`](https://github.com/MPW1412/avm-fritz3390-802.11p-otm)
ships the four mac80211/ath9k/wireless-regdb patches needed to make this
happen on stock OpenWrt 25.12; the patches are non-invasive and should
apply against other ath9k targets (PC Engines APU, Mikrotik R11e-2HnD,
generic mPCIe Atheros cards) with minor adjustments.

## Limitations

* Receive-only by default. The OCB mode for transmitting V2X frames is
  available in the underlying ath9k driver but is **not** wired up here —
  monitor mode is enough to feed OTM and avoids any regulatory questions
  about transmitting on 5,9 GHz.
* No CAM/DENM decoding on the device. Frames are forwarded raw; OTM's
  backend does the ASN.1 UPER parsing.

## License

[WTFPL](http://www.wtfpl.net/) (Do What The Fuck You Want To Public License).
See `LICENSE`.

The package depends at runtime on libpcap (BSD), libmosquitto (EPL/EDL),
mbedTLS (Apache-2.0) and OpenWrt's package framework (GPL-2.0). Those
projects keep their own licenses.

## Acknowledgements

* [opentrafficmap.org](https://opentrafficmap.org/) and the
  [ESP32-C5 receiver firmware](https://codeberg.org/opentrafficmap/its-g5-receiver-firmware)
  for the wire format / broker.
* [Francesco Raviglione](https://github.com/francescoraves483/OpenWrt-V2X)
  for the original V2X patches we forward-ported.
* The
  [HPI Potsdam 11p-on-linux project](https://gitlab.com/hpi-potsdam/osm/g5-on-linux/11p-on-linux)
  for documenting the Linux 802.11p story.
