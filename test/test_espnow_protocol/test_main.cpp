// On-device tests for the ESP-NOW connection/auth contract: frame validation,
// the handshake crypto, and the gateway's session table. They need no radio
// or second device -- peer changes go to fakes.
//
//   pio test -e esp32-s3-devkitc1-n16r8 -f test_espnow_protocol
//
// The end-to-end parts (encrypted PING/PONG over the air, resync after a
// reconnect, gateway reboot) need a gateway and a client: see the manual
// checklist in NightMareNetwork's docs/modules/espnow-protocol.md.

#include <unity.h>
#include <string.h>

// The units under test, compiled straight in: the test build does not link src/.
#include "../../src/NightmareGateway/EspNow/Frame.cpp"
#include "../../src/NightmareGateway/EspNow/Auth.cpp"
#include "../../src/NightmareGateway/EspNow/macAddress.cpp"
#include "../../src/NightmareGateway/NightMare/Topic.cpp"
#include "../../src/NightmareGateway/NightMare/Message.cpp"
#include "../../src/NightmareGateway/NightMare/Device.cpp"
#include "../../src/NightmareGateway/EspNow/espDeviceManager.cpp"

using namespace NightMare;
namespace A = NightMare::EspNowAuth;

// --- Helpers -------------------------------------------------------------------

static const uint8_t PSK[] = "nightmare-test-psk-0123456789";
static constexpr size_t PSK_LEN = sizeof(PSK) - 1;

static A::HandshakeContext vectorContext()
{
    A::HandshakeContext c{};
    const uint8_t client[6] = {0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33};
    const uint8_t gateway[6] = {0x7C, 0xDF, 0xA1, 0x44, 0x55, 0x66};
    memcpy(c.clientMac, client, 6);
    memcpy(c.gatewayMac, gateway, 6);
    c.clientNonce = 0x0102030405060708ULL;
    c.gatewayNonce = 0x1122334455667788ULL;
    return c;
}

// Raw packet: header fields as given, `length` data bytes of 0xAA.
static size_t packet(uint8_t *out, uint8_t type, uint16_t cid, uint16_t length,
                     uint8_t version = CurrentFrameVersion, uint8_t index = 0, uint8_t total = 1)
{
    FrameHeader h{};
    h.messageId = 7;
    h.version = version;
    h.type = type;
    h.cid = cid;
    h.frameIndex = index;
    h.totalFrames = total;
    h.length = length;
    memcpy(out, &h, sizeof(h));
    memset(out + sizeof(h), 0xAA, length);
    return sizeof(h) + length;
}

static FrameCheck check(uint8_t type, uint16_t cid, uint16_t length, uint8_t version = CurrentFrameVersion,
                        uint8_t index = 0, uint8_t total = 1)
{
    uint8_t buffer[MaxPacketSizeV1];
    const size_t size = packet(buffer, type, cid, length, version, index, total);
    return validateFrame(buffer, size);
}

static FrameHeader header(FrameType type, uint16_t cid)
{
    FrameHeader h{};
    h.version = CurrentFrameVersion;
    h.type = (uint8_t)type;
    h.cid = cid;
    h.totalFrames = 1;
    return h;
}

// Fake peer table.
static int s_peersAdded, s_peersSecured, s_peersRemoved;
static bool s_secureFails;
static uint8_t s_lastLmk[16];
static bool fakeAdd(const MacAddress &) { s_peersAdded++; return true; }
static bool fakeSecure(const MacAddress &, const uint8_t *lmk)
{
    if (s_secureFails)
        return false;
    s_peersSecured++;
    memcpy(s_lastLmk, lmk, 16);
    return true;
}
static void fakeRemove(const MacAddress &) { s_peersRemoved++; }

static int s_lost;
static void onLost(Device &) { s_lost++; }

static MacAddress mac(uint8_t last)
{
    const uint8_t raw[6] = {0x10, 0x20, 0x30, 0x40, 0x50, last};
    return MacAddress(raw);
}

static espDeviceManager manager(uint16_t firstCid = 1)
{
    espDeviceManager m(60000, firstCid);
    espDeviceManager::PeerOps ops;
    ops.add = fakeAdd;
    ops.secure = fakeSecure;
    ops.remove = fakeRemove;
    m.setPeerOps(ops);
    return m;
}

// Runs CONNECT..secure for `who`, leaving a CONNECTED session.
static Device *connect(espDeviceManager &m, const MacAddress &who, uint64_t now = 0)
{
    const uint8_t lmk[16] = {1};
    if (m.beginHandshake(who, 1, 0, 2, now) == nullptr)
        return nullptr;
    Device *d = m.completeHandshake(who, now);
    if (d == nullptr || !m.secureSession(*d, lmk))
        return nullptr;
    d->setState(ConnectionState::CONNECTED);
    return d;
}

void setUp(void)
{
    s_peersAdded = s_peersSecured = s_peersRemoved = 0;
    s_secureFails = false;
    s_lost = 0;
}

void tearDown(void) {}

// --- Frame -------------------------------------------------------------------

static void test_header_is_10_bytes(void)
{
    TEST_ASSERT_EQUAL(10, sizeof(FrameHeader));
    TEST_ASSERT_EQUAL(240, MaxFrameDataSize);
}

static void test_version_nibbles(void)
{
    const uint8_t v = makeVersion(0x1, 0x3);
    TEST_ASSERT_EQUAL_HEX8(0x13, v);
    TEST_ASSERT_EQUAL(1, espNowVersion(v));
    TEST_ASSERT_EQUAL(3, nmProtocolVersion(v));
    TEST_ASSERT_EQUAL(0, espNowVersion(makeVersion((uint8_t)EspNowFrameVersion::V1, 0)));
    TEST_ASSERT_EQUAL(1, espNowVersion(makeVersion((uint8_t)EspNowFrameVersion::V2, 0)));
    TEST_ASSERT_EQUAL(0, NM_PROTOCOL_VERSION);
    TEST_ASSERT_EQUAL_HEX8(0x00, CurrentFrameVersion);
}

static void test_valid_frames_pass(void)
{
    TEST_ASSERT_EQUAL(FrameCheck::OK, check((uint8_t)FrameType::CONNECT, 0, sizeof(ConnectPayload)));
    TEST_ASSERT_EQUAL(FrameCheck::OK, check((uint8_t)FrameType::CONNACK, 5, sizeof(ConnAckPayload)));
    TEST_ASSERT_EQUAL(FrameCheck::OK, check((uint8_t)FrameType::MESSAGE, 5, 240, CurrentFrameVersion, 3, 16));
    TEST_ASSERT_EQUAL(FrameCheck::OK, check((uint8_t)FrameType::ERROR, 0, 1));

    Frame f{};
    TEST_ASSERT_TRUE(makeFrame(f, FrameType::PING, 9, 42));
    TEST_ASSERT_EQUAL(FrameCheck::OK, validateFrame((const uint8_t *)&f, frameSize(f)));
    TEST_ASSERT_FALSE(makeFrame(f, FrameType::MESSAGE, 9, 42, PSK, MaxFrameDataSize + 1));
}

static void test_malformed_length_rejected(void)
{
    uint8_t buffer[MaxPacketSizeV1];
    const size_t size = packet(buffer, (uint8_t)FrameType::MESSAGE, 5, 20);
    TEST_ASSERT_EQUAL(FrameCheck::BAD_LENGTH, validateFrame(buffer, size - 1)); // claims more than arrived
    TEST_ASSERT_EQUAL(FrameCheck::BAD_LENGTH, validateFrame(buffer, size + 1)); // trailing bytes
    TEST_ASSERT_EQUAL(FrameCheck::TOO_SHORT, validateFrame(buffer, 9));
    TEST_ASSERT_EQUAL(FrameCheck::TOO_SHORT, validateFrame(nullptr, 20));
}

static void test_fragment_rules(void)
{
    const uint8_t msg = (uint8_t)FrameType::MESSAGE;
    TEST_ASSERT_EQUAL(FrameCheck::BAD_FRAGMENT, check(msg, 5, 1, CurrentFrameVersion, 0, 0));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_FRAGMENT, check(msg, 5, 1, CurrentFrameVersion, 2, 2));
    TEST_ASSERT_EQUAL(FrameCheck::TOO_MANY_FRAGMENTS, check(msg, 5, 1, CurrentFrameVersion, 0, 17));
    // Only MESSAGE may be fragmented.
    TEST_ASSERT_EQUAL(FrameCheck::BAD_FRAGMENT, check((uint8_t)FrameType::SUBSCRIBE, 5, 1, CurrentFrameVersion, 0, 2));
}

static void test_versions_rejected(void)
{
    const uint8_t ping = (uint8_t)FrameType::PING;
    TEST_ASSERT_EQUAL(FrameCheck::UNSUPPORTED_PROTOCOL, check(ping, 5, 0, makeVersion(0, 1)));
    TEST_ASSERT_EQUAL(FrameCheck::UNSUPPORTED_FRAMING, check(ping, 5, 0, makeVersion(1, 0)));
    TEST_ASSERT_EQUAL(FrameCheck::UNKNOWN_TYPE, check(14, 5, 0));
}

static void test_cid_semantics(void)
{
    TEST_ASSERT_EQUAL(FrameCheck::BAD_CID, check((uint8_t)FrameType::CONNECT, 3, sizeof(ConnectPayload)));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_CID, check((uint8_t)FrameType::AUTH, 3, sizeof(AuthPayload)));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_CID, check((uint8_t)FrameType::CONNACK, 0, sizeof(ConnAckPayload)));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_CID, check((uint8_t)FrameType::MESSAGE, 0, 5));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_CID, check((uint8_t)FrameType::PING, 0, 0));
}

static void test_fixed_payload_sizes(void)
{
    // A short AUTH never reaches the handshake: nothing half-authenticated can follow.
    TEST_ASSERT_EQUAL(FrameCheck::BAD_PAYLOAD, check((uint8_t)FrameType::AUTH, 0, sizeof(AuthPayload) - 1));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_PAYLOAD, check((uint8_t)FrameType::CONNECT, 0, sizeof(ConnectPayload) + 1));
    TEST_ASSERT_EQUAL(FrameCheck::BAD_PAYLOAD, check((uint8_t)FrameType::PING, 5, 1));
}

static void test_beacon(void)
{
    const Frame beacon = beaconFrame();
    TEST_ASSERT_EQUAL(FrameCheck::OK, validateFrame((const uint8_t *)&beacon, frameSize(beacon)));
    TEST_ASSERT_EQUAL(0, beacon.header.cid);
    TEST_ASSERT_TRUE(beaconSupports(beacon, EspNowFrameVersion::V1));
    TEST_ASSERT_FALSE(beaconSupports(beacon, EspNowFrameVersion::V2));
}

// --- Crypto ------------------------------------------------------------------

// Vectors computed independently (Python hmac/hashlib) from the contract in
// Auth.h, so both ends are checked against the spec, not against each other.
static void test_known_answer_vectors(void)
{
    const uint8_t authExpected[16] = {0xB0, 0xF3, 0x83, 0x2A, 0x3B, 0x83, 0x21, 0x4B,
                                      0xAA, 0x7C, 0xFA, 0xBC, 0xC7, 0x9C, 0xB8, 0x5D};
    const uint8_t lmkExpected[16] = {0xA7, 0xB7, 0xD7, 0xD5, 0xE9, 0x99, 0xB4, 0x23,
                                     0x3A, 0x75, 0x1D, 0x4A, 0x7A, 0x54, 0x78, 0xBB};
    const A::HandshakeContext c = vectorContext();
    uint8_t proof[16], lmk[16];
    TEST_ASSERT_TRUE(A::authProof(PSK, PSK_LEN, c, proof));
    TEST_ASSERT_TRUE(A::sessionLmk(PSK, PSK_LEN, c, lmk));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(authExpected, proof, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lmkExpected, lmk, 16);
}

static void test_proof_and_lmk_differ(void)
{
    const A::HandshakeContext c = vectorContext();
    uint8_t proof[16], lmk[16];
    A::authProof(PSK, PSK_LEN, c, proof);
    A::sessionLmk(PSK, PSK_LEN, c, lmk);
    TEST_ASSERT_FALSE(memcmp(proof, lmk, 16) == 0);
}

static void test_every_input_changes_the_result(void)
{
    const A::HandshakeContext base = vectorContext();
    uint8_t reference[16], other[16];
    A::sessionLmk(PSK, PSK_LEN, base, reference);

    A::HandshakeContext c = base;
    c.clientNonce ^= 1;
    A::sessionLmk(PSK, PSK_LEN, c, other);
    TEST_ASSERT_FALSE(memcmp(reference, other, 16) == 0);

    c = base;
    c.gatewayNonce ^= 1;
    A::sessionLmk(PSK, PSK_LEN, c, other);
    TEST_ASSERT_FALSE(memcmp(reference, other, 16) == 0);

    c = base;
    c.clientMac[5] ^= 1;
    A::sessionLmk(PSK, PSK_LEN, c, other);
    TEST_ASSERT_FALSE(memcmp(reference, other, 16) == 0);

    c = base;
    c.gatewayMac[0] ^= 1;
    A::sessionLmk(PSK, PSK_LEN, c, other);
    TEST_ASSERT_FALSE(memcmp(reference, other, 16) == 0);

    // Both ends derive the same key from the same inputs.
    A::sessionLmk(PSK, PSK_LEN, base, other);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(reference, other, 16);
}

static void test_auth_rejections(void)
{
    const A::HandshakeContext c = vectorContext();
    uint8_t expected[16], proof[16];
    A::authProof(PSK, PSK_LEN, c, expected);

    // Wrong PSK.
    const uint8_t wrong[] = "nightmare-test-psk-0123456780";
    A::authProof(wrong, sizeof(wrong) - 1, c, proof);
    TEST_ASSERT_FALSE(A::constantTimeEqual(expected, proof, 16));

    // Replayed AUTH: a proof from an earlier handshake does not satisfy a new
    // challenge (fresh gateway nonce).
    A::HandshakeContext next = c;
    next.gatewayNonce = 0xDEADBEEFULL;
    A::authProof(PSK, PSK_LEN, next, proof);
    TEST_ASSERT_FALSE(A::constantTimeEqual(expected, proof, 16));

    // Sender MAC is part of the proof: a different sender cannot reuse it.
    A::HandshakeContext spoof = c;
    spoof.clientMac[0] ^= 0x02;
    A::authProof(PSK, PSK_LEN, spoof, proof);
    TEST_ASSERT_FALSE(A::constantTimeEqual(expected, proof, 16));

    TEST_ASSERT_TRUE(A::constantTimeEqual(expected, expected, 16));
    TEST_ASSERT_FALSE(A::authProof(nullptr, 0, c, proof));
}

static void test_nonces_are_fresh(void)
{
    const uint64_t a = A::freshNonce();
    const uint64_t b = A::freshNonce();
    TEST_ASSERT_FALSE(a == b);
}

// --- Sessions ----------------------------------------------------------------

static void test_unknown_sender_creates_nothing(void)
{
    espDeviceManager m = manager();
    for (FrameType t : {FrameType::MESSAGE, FrameType::SUBSCRIBE, FrameType::UNSUBSCRIBE, FrameType::LAST_WILL,
                        FrameType::PING, FrameType::PONG, FrameType::ACK, FrameType::DISCONNECT, FrameType::AUTH})
        TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(1), header(t, t == FrameType::AUTH ? 0 : 5)).verdict);
    TEST_ASSERT_EQUAL(0, m.getDeviceCount());
    TEST_ASSERT_EQUAL(0, m.pendingCount());
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::HANDSHAKE, m.admit(mac(1), header(FrameType::CONNECT, 0)).verdict);
}

static void test_cid_only_after_auth(void)
{
    espDeviceManager m = manager();
    TEST_ASSERT_NOT_NULL(m.beginHandshake(mac(1), 11, 0, 22, 0));
    TEST_ASSERT_EQUAL(0, m.getDeviceCount());
    TEST_ASSERT_EQUAL(1, m.pendingCount());
    TEST_ASSERT_EQUAL(1, s_peersAdded);
    // Pending: AUTH is admitted, normal traffic is not.
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::HANDSHAKE, m.admit(mac(1), header(FrameType::AUTH, 0)).verdict);
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(1), header(FrameType::MESSAGE, 1)).verdict);

    Device *d = m.completeHandshake(mac(1), 0);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_NOT_EQUAL(0, d->cid());
    TEST_ASSERT_EQUAL(ConnectionState::AUTHENTICATED, d->state());
    TEST_ASSERT_EQUAL(0, m.pendingCount());
    TEST_ASSERT_NULL(m.pendingFor(mac(1)));
}

static void test_securing_gate(void)
{
    espDeviceManager m = manager();
    const uint8_t lmk[16] = {9, 8, 7};
    m.beginHandshake(mac(1), 1, 0, 2, 0);
    Device *d = m.completeHandshake(mac(1), 0);
    TEST_ASSERT_TRUE(m.secureSession(*d, lmk));
    TEST_ASSERT_EQUAL(ConnectionState::SECURING, d->state());
    TEST_ASSERT_EQUAL(1, s_peersSecured);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(lmk, s_lastLmk, 16);

    const uint16_t cid = d->cid();
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::SESSION, m.admit(mac(1), header(FrameType::PING, cid)).verdict);
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::NOT_CONNECTED, m.admit(mac(1), header(FrameType::MESSAGE, cid)).verdict);
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::NOT_CONNECTED, m.admit(mac(1), header(FrameType::SUBSCRIBE, cid)).verdict);
}

static void test_failed_encryption_is_not_connected(void)
{
    espDeviceManager m = manager();
    const uint8_t lmk[16] = {1};
    m.beginHandshake(mac(1), 1, 0, 2, 0);
    Device *d = m.completeHandshake(mac(1), 0);
    s_secureFails = true;
    TEST_ASSERT_FALSE(m.secureSession(*d, lmk));
    TEST_ASSERT_EQUAL(0, m.getDeviceCount());
    TEST_ASSERT_NULL(m.sessionFor(mac(1)));
}

static void test_connected_mac_and_cid(void)
{
    espDeviceManager m = manager();
    Device *d = connect(m, mac(1));
    TEST_ASSERT_NOT_NULL(d);
    const uint16_t cid = d->cid();

    espDeviceManager::Admission ok = m.admit(mac(1), header(FrameType::MESSAGE, cid));
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::SESSION, ok.verdict);
    TEST_ASSERT_EQUAL_PTR(d, ok.device);
    // Right MAC, stale cid.
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::INVALID_SESSION, m.admit(mac(1), header(FrameType::MESSAGE, cid + 1)).verdict);
    // Valid cid, wrong MAC.
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(2), header(FrameType::MESSAGE, cid)).verdict);
    // Handshake-only types are never session traffic.
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(1), header(FrameType::CONNACK, cid)).verdict);
}

static void test_reconnect_gets_new_cid(void)
{
    espDeviceManager m = manager();
    const uint16_t first = connect(m, mac(1))->cid();
    // A new CONNECT from the same MAC ends the old session (no last will).
    m.beginHandshake(mac(1), 3, 0, 4, 10);
    TEST_ASSERT_NULL(m.sessionFor(mac(1)));
    TEST_ASSERT_EQUAL(0, s_lost);
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(1), header(FrameType::MESSAGE, first)).verdict);
    Device *d = m.completeHandshake(mac(1), 10);
    TEST_ASSERT_NOT_EQUAL(first, d->cid());
}

static void test_gateway_reboot_invalidates_cids(void)
{
    uint16_t old;
    {
        espDeviceManager before = manager();
        old = connect(before, mac(1))->cid();
    }
    espDeviceManager after = manager(); // a fresh table, as after a reboot
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, after.admit(mac(1), header(FrameType::MESSAGE, old)).verdict);
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, after.admit(mac(1), header(FrameType::PING, old)).verdict);
}

static void test_cids_unique_nonzero_and_wrap(void)
{
    espDeviceManager m = manager(0xFFFF);
    const uint16_t a = connect(m, mac(1))->cid();
    const uint16_t b = connect(m, mac(2))->cid();
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, a);
    TEST_ASSERT_EQUAL_HEX16(0x0001, b); // skips 0
    TEST_ASSERT_EQUAL_PTR(m.sessionFor(mac(2)), m.sessionByCid(b));
    TEST_ASSERT_NULL(m.sessionByCid(0));
}

static void test_pending_timeout_frees_state(void)
{
    espDeviceManager m = manager();
    m.beginHandshake(mac(1), 1, 0, 2, 1000);
    m.expire(1000 + espDeviceManager::HandshakeTimeoutMs, onLost);
    TEST_ASSERT_EQUAL(1, m.pendingCount());
    m.expire(1001 + espDeviceManager::HandshakeTimeoutMs, onLost);
    TEST_ASSERT_EQUAL(0, m.pendingCount());
    TEST_ASSERT_EQUAL(1, s_peersRemoved);
    // An expired challenge can no longer be answered.
    TEST_ASSERT_EQUAL(espDeviceManager::Verdict::IGNORE, m.admit(mac(1), header(FrameType::AUTH, 0)).verdict);
}

static void test_pending_table_is_bounded(void)
{
    espDeviceManager m = manager();
    for (uint8_t i = 1; i <= espDeviceManager::MaxPending; i++)
        TEST_ASSERT_NOT_NULL(m.beginHandshake(mac(i), i, 0, i, 0));
    TEST_ASSERT_NULL(m.beginHandshake(mac(99), 1, 0, 1, 0));
    // A repeated CONNECT reuses its own slot rather than growing the table.
    TEST_ASSERT_NOT_NULL(m.beginHandshake(mac(1), 5, 0, 6, 0));
    TEST_ASSERT_EQUAL(espDeviceManager::MaxPending, m.pendingCount());
    // Once one is stale, a newcomer takes its slot.
    TEST_ASSERT_NOT_NULL(m.beginHandshake(mac(99), 1, 0, 1, espDeviceManager::HandshakeTimeoutMs + 1));
}

static void test_stuck_securing_ends_without_will(void)
{
    espDeviceManager m = manager();
    const uint8_t lmk[16] = {1};
    m.beginHandshake(mac(1), 1, 0, 2, 0);
    Device *d = m.completeHandshake(mac(1), 0);
    Message will;
    will.topic = "dev/status";
    d->setLastWill(will);
    m.secureSession(*d, lmk);
    m.expire(espDeviceManager::SecuringTimeoutMs + 1, onLost);
    TEST_ASSERT_EQUAL(0, m.getDeviceCount());
    TEST_ASSERT_EQUAL(0, s_lost);
}

static void test_silent_session_fires_will(void)
{
    espDeviceManager m = manager();
    Device *d = connect(m, mac(1), 0);
    Message will;
    will.topic = "dev/status";
    TEST_ASSERT_TRUE(d->setLastWill(will));
    m.expire(60000, onLost);
    TEST_ASSERT_EQUAL(1, m.getDeviceCount());
    m.expire(60001, onLost);
    TEST_ASSERT_EQUAL(0, m.getDeviceCount());
    TEST_ASSERT_EQUAL(1, s_lost);
}

static void test_subscribers_count_connected_only(void)
{
    espDeviceManager m = manager();
    Device *d = connect(m, mac(1));
    d->subscribeTo("a/#");
    TEST_ASSERT_EQUAL(1, m.getSubscriberCount());
    d->setState(ConnectionState::SECURING);
    TEST_ASSERT_EQUAL(0, m.getSubscriberCount());
}

extern "C" void app_main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_header_is_10_bytes);
    RUN_TEST(test_version_nibbles);
    RUN_TEST(test_valid_frames_pass);
    RUN_TEST(test_malformed_length_rejected);
    RUN_TEST(test_fragment_rules);
    RUN_TEST(test_versions_rejected);
    RUN_TEST(test_cid_semantics);
    RUN_TEST(test_fixed_payload_sizes);
    RUN_TEST(test_beacon);
    RUN_TEST(test_known_answer_vectors);
    RUN_TEST(test_proof_and_lmk_differ);
    RUN_TEST(test_every_input_changes_the_result);
    RUN_TEST(test_auth_rejections);
    RUN_TEST(test_nonces_are_fresh);
    RUN_TEST(test_unknown_sender_creates_nothing);
    RUN_TEST(test_cid_only_after_auth);
    RUN_TEST(test_securing_gate);
    RUN_TEST(test_failed_encryption_is_not_connected);
    RUN_TEST(test_connected_mac_and_cid);
    RUN_TEST(test_reconnect_gets_new_cid);
    RUN_TEST(test_gateway_reboot_invalidates_cids);
    RUN_TEST(test_cids_unique_nonzero_and_wrap);
    RUN_TEST(test_pending_timeout_frees_state);
    RUN_TEST(test_pending_table_is_bounded);
    RUN_TEST(test_stuck_securing_ends_without_will);
    RUN_TEST(test_silent_session_fires_will);
    RUN_TEST(test_subscribers_count_connected_only);
    UNITY_END();
}
