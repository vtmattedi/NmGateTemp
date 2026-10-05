# Nightmare Gateway network contract

The gateway is a first-class NMNW device. Its stable id is derived from the
station MAC as `nmnw-gateway-<twelve-hex-digit-mac>` and is used by both its
MQTT topics and the ESP-NOW beacon. A random 64-bit hexadecimal `generation`
is created once per boot. Generation values are opaque equality tokens, not
ordered counters.

The frozen retained MQTT topics are:

```text
<gateway-id>/status
<gateway-id>/gateway/network
<gateway-id>/gateway/clients
```

`status` is the normal presence document. Its MQTT LWT has `online=false` and
the current generation. `gateway/network` contains schema 1 identity,
capabilities, radio (`channel`, `ssid`, `bssid`) and remote/local MQTT uplink
readiness. `gateway/clients` is a schema 1 projection of `espDeviceManager`;
it does not maintain another liveness table. Each client contains its MAC id,
learned name, connected state, and LastWill topic, byte-array payload, and
retained flag.

On MQTT connection the gateway synchronously publishes its locally-owned
retained vault, current status, network state, and client snapshot before it
subscribes to `#`. Consequently retained broker replay cannot precede the
gateway-owned truth. A retained device status from MQTT is ignored while the
same named device has a live authenticated ESP-NOW session.

Every bridge also implements the backend recovery projection. It caches
retained client snapshots from other gateways. A matching-generation gateway
LWT applies those LastWills once. Repeated LWT delivery is idempotent, and an
offline event from an older generation cannot invalidate a newer online
generation.

The gateway id in an ESP-NOW beacon is discovery correlation only. The PSK
handshake and encrypted session remain the authentication boundary.
