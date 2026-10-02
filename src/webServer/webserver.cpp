#include "webserver.h"
#include "EspWifi/espWifi.h"
#include "NightmareGateway/NightmareGateway.h"
#include "NightmareGateway/EspMqtt/espMqtt.h"
#include "NightmareGateway/EspNow/espBroker.h"
#include "NightmareGateway/GatewayStats.h"
#include "System/chipTemperature.h"

// ESP-IDF HTTP server exposing device state and internal runtime metrics.
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "cJSON.h"
#include "Version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace web_server {

static const char *TAG = "web_server";
static httpd_handle_t s_server = nullptr;

struct SystemState {
	NightMareGatewayState gateway_state;
	bool wifi_started;
	bool wifi_connected;
	bool mqtt_connected;
	bool beacon_active;
	uint8_t device_count;
	uint8_t subscriber_count;
	uint32_t seconds_since_last_beacon;
	bool temperature_available;
	float temperature_c;
};

static SystemState read_system_state()
{
	SystemState state = {};
	state.gateway_state = nightmare_gateway_state();
	state.wifi_started = wifi_is_started();
	state.wifi_connected = wifi_is_connected();
	state.mqtt_connected = mqtt_is_connected();
	state.beacon_active = espBroker_beaconActive();
	state.device_count = espBroker_deviceCount();
	state.subscriber_count = espBroker_subscriberCount();
	state.seconds_since_last_beacon = espBroker_secondsSinceLastBeacon();
	state.temperature_available = chipTempRead(state.temperature_c);
	return state;
}

static bool request_text_format(httpd_req_t *request)
{
	char query[64] = {};
	char format[16] = {};
	if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
		httpd_query_key_value(query, "format", format, sizeof(format)) == ESP_OK &&
		strcmp(format, "text") == 0) {
		return true;
	}

	char accept[64] = {};
	return httpd_req_get_hdr_value_str(request, "Accept", accept, sizeof(accept)) == ESP_OK &&
		   strstr(accept, "text/plain") != nullptr &&
		   strstr(accept, "application/json") == nullptr;
}

static esp_err_t send_json(httpd_req_t *request, cJSON *root)
{
	char *payload = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (payload == nullptr) {
		return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
								   "Unable to allocate JSON response");
	}

	httpd_resp_set_type(request, "application/json");
	esp_err_t result = httpd_resp_send(request, payload, HTTPD_RESP_USE_STRLEN);
	free(payload);
	return result;
}

// --- Web UI --------------------------------------------------------------------
// The files in src/webServer/WebSite are linked into the firmware (EMBED_FILES
// in src/CMakeLists.txt), so editing them only needs a rebuild + flash.

#define EMBEDDED_FILE(name, symbol)                                         \
	extern const uint8_t name##_start[] asm("_binary_" symbol "_start");    \
	extern const uint8_t name##_end[] asm("_binary_" symbol "_end")

EMBEDDED_FILE(index_html, "index_html");
EMBEDDED_FILE(style_css, "style_css");
EMBEDDED_FILE(app_js, "app_js");
EMBEDDED_FILE(logo_svg, "nightmare_logo_svg");

struct EmbeddedFile {
	const char *uri;
	const char *content_type;
	const uint8_t *start;
	const uint8_t *end;
};

static const EmbeddedFile s_files[] = {
	{"/", "text/html; charset=utf-8", index_html_start, index_html_end},
	{"/index.html", "text/html; charset=utf-8", index_html_start, index_html_end},
	{"/style.css", "text/css; charset=utf-8", style_css_start, style_css_end},
	{"/app.js", "text/javascript; charset=utf-8", app_js_start, app_js_end},
	{"/nightmare_logo.svg", "image/svg+xml", logo_svg_start, logo_svg_end},
};

static esp_err_t file_handler(httpd_req_t *request)
{
	const EmbeddedFile *file = static_cast<const EmbeddedFile *>(request->user_ctx);
	httpd_resp_set_type(request, file->content_type);
	// Revalidate every load: the UI changes with the firmware, not on a timer.
	httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
	return httpd_resp_send(request, reinterpret_cast<const char *>(file->start),
						   static_cast<ssize_t>(file->end - file->start));
}

static void add_counter(cJSON *object, const char *name, const std::atomic<uint32_t> &counter)
{
	cJSON_AddNumberToObject(object, name, counter.load(std::memory_order_relaxed));
}

static void add_wifi(cJSON *root)
{
	cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
	cJSON_AddBoolToObject(wifi, "connected", wifi_is_connected());

	wifi_ap_record_t ap = {};
	if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
		cJSON_AddStringToObject(wifi, "ssid", reinterpret_cast<const char *>(ap.ssid));
		cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
		cJSON_AddNumberToObject(wifi, "channel", ap.primary);
	}

	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t ip = {};
	if (netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
		char text[16];
		snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
		cJSON_AddStringToObject(wifi, "ip", text);
	}
}

static void add_devices(cJSON *root, uint64_t now_ms)
{
	std::vector<EspBrokerDeviceInfo> devices;
	espBroker_snapshotDevices(devices);

	cJSON *list = cJSON_AddArrayToObject(root, "devices");
	for (const EspBrokerDeviceInfo &device : devices) {
		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "mac", device.mac.c_str());
		cJSON_AddStringToObject(item, "name", device.name.c_str());
		cJSON_AddNumberToObject(item, "cid", device.cid);
		cJSON_AddStringToObject(item, "state", NightMare::connectionStateName(device.state));
		// Only a verified AUTH creates a session, so every entry here has authenticated.
		cJSON_AddBoolToObject(item, "authenticated", true);
		cJSON_AddBoolToObject(item, "secured", device.state == NightMare::ConnectionState::CONNECTED);
		cJSON_AddBoolToObject(item, "suspended", device.suspended);
		cJSON_AddNumberToObject(item, "subs", device.subscriptions.size());
		cJSON *topics = cJSON_AddArrayToObject(item, "topics");
		for (const std::string &topic : device.subscriptions)
			cJSON_AddItemToArray(topics, cJSON_CreateString(topic.c_str()));
		cJSON_AddBoolToObject(item, "last_will", device.hasLastWill);
		if (device.hasLastWill) {
			cJSON_AddStringToObject(item, "last_will_topic", device.lastWillTopic.c_str());
			cJSON_AddNumberToObject(item, "last_will_size", device.lastWillPayloadSize);
		}
		if (device.hasRssi) {
			cJSON_AddNumberToObject(item, "rssi", device.rssi);
			cJSON_AddNumberToObject(item, "avg_rssi", static_cast<int>(device.avgRssi * 10.0f) / 10.0);
		} else {
			cJSON_AddNullToObject(item, "rssi");
			cJSON_AddNullToObject(item, "avg_rssi");
		}
		if (device.hasRtt)
			cJSON_AddNumberToObject(item, "rtt_ms", static_cast<int>(device.rttMs * 10.0f) / 10.0);
		else
			cJSON_AddNullToObject(item, "rtt_ms");
		cJSON_AddNumberToObject(item, "rx_frames", device.rxFrames);
		cJSON_AddNumberToObject(item, "last_seen_ms",
								static_cast<double>(now_ms > device.lastSeenMs ? now_ms - device.lastSeenMs : 0));
		cJSON_AddNumberToObject(item, "session_ms",
								static_cast<double>(now_ms > device.sessionStartMs ? now_ms - device.sessionStartMs : 0));
		cJSON_AddBoolToObject(item, "disconnect_candidate", device.disconnectCandidate);
		cJSON_AddItemToArray(list, item);
	}
}

// Everything the dashboard polls once a second, in a single response so the
// ESP serves one request instead of five. Counters are cumulative: the page
// derives rates from two samples and `uptime_ms`.
static esp_err_t live_handler(httpd_req_t *request)
{
	const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
	const SystemState state = read_system_state();

	cJSON *root = cJSON_CreateObject();
	if (root == nullptr) {
		return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
								   "Unable to allocate JSON response");
	}
	cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(now_ms));

	cJSON *gateway = cJSON_AddObjectToObject(root, "gateway");
	cJSON_AddStringToObject(gateway, "state", nightmare_gateway_state_name(state.gateway_state));
	cJSON_AddBoolToObject(gateway, "ready", state.gateway_state == NightMareGatewayState::Running);
	cJSON_AddStringToObject(gateway, "version", VERSION);
	cJSON_AddStringToObject(gateway, "build", BUILD_TIMESTAMP);

	add_wifi(root);
	cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
	cJSON_AddBoolToObject(mqtt, "connected", state.mqtt_connected);

	cJSON *system = cJSON_AddObjectToObject(root, "system");
	cJSON_AddNumberToObject(system, "free_heap", esp_get_free_heap_size());
	cJSON_AddNumberToObject(system, "min_free_heap", esp_get_minimum_free_heap_size());
	cJSON_AddNumberToObject(system, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
	if (state.temperature_available)
		cJSON_AddNumberToObject(system, "temperature_c", state.temperature_c);
	else
		cJSON_AddNullToObject(system, "temperature_c");

	cJSON *esp_now = cJSON_AddObjectToObject(root, "espnow");
	cJSON_AddBoolToObject(esp_now, "beacon_active", state.beacon_active);
	if (state.seconds_since_last_beacon == UINT32_MAX)
		cJSON_AddNullToObject(esp_now, "seconds_since_last_beacon");
	else
		cJSON_AddNumberToObject(esp_now, "seconds_since_last_beacon", state.seconds_since_last_beacon);
	cJSON_AddNumberToObject(esp_now, "devices", state.device_count);
	cJSON_AddNumberToObject(esp_now, "subscribers", state.subscriber_count);
	cJSON_AddNumberToObject(esp_now, "heartbeat_ms", espBroker_heartbeatMs());
	cJSON_AddNumberToObject(esp_now, "session_timeout_ms", espBroker_sessionTimeoutMs());
	add_counter(esp_now, "rx_packets", g_gatewayStats.espnowRxPackets);
	add_counter(esp_now, "tx_packets", g_gatewayStats.espnowTxPackets);
	add_counter(esp_now, "rx_bytes", g_gatewayStats.espnowRxBytes);
	add_counter(esp_now, "tx_bytes", g_gatewayStats.espnowTxBytes);
	add_counter(esp_now, "tx_failed", g_gatewayStats.espnowTxFailed);
	add_counter(esp_now, "rx_dropped", g_gatewayStats.espnowRxDropped);
	add_counter(esp_now, "invalid_frames", g_gatewayStats.invalidFrames);
	add_counter(esp_now, "ignored_frames", g_gatewayStats.ignoredFrames);

	// "local" is the ESP-NOW side, "remote" the MQTT broker.
	cJSON *messages = cJSON_AddObjectToObject(root, "messages");
	add_counter(messages, "from_local", g_gatewayStats.msgsFromLocal);
	add_counter(messages, "to_local", g_gatewayStats.msgsToLocal);
	add_counter(messages, "from_remote", g_gatewayStats.msgsFromRemote);
	add_counter(messages, "to_remote", g_gatewayStats.msgsToRemote);

	cJSON *vault = cJSON_AddObjectToObject(root, "vault");
	cJSON_AddNumberToObject(vault, "count", nightmare_gateway_vault_size());

	add_devices(root, now_ms);
	return send_json(request, root);
}

// Payloads are shown as text when they are valid UTF-8 without control
// characters, and as hex when they are not.
static bool is_text(const std::vector<uint8_t> &payload)
{
	size_t i = 0;
	while (i < payload.size()) {
		const uint8_t c = payload[i];
		size_t extra = 0;
		if (c < 0x20) {
			if (c != '\t' && c != '\n' && c != '\r')
				return false;
		} else if (c == 0x7F) {
			return false;
		} else if (c >= 0xF0 && c <= 0xF4) {
			extra = 3;
		} else if (c >= 0xE0 && c < 0xF0) {
			extra = 2;
		} else if (c >= 0xC2 && c < 0xE0) {
			extra = 1;
		} else if (c >= 0x80) {
			return false;
		}
		if (i + extra >= payload.size() && extra != 0)
			return false;
		for (size_t k = 1; k <= extra; k++) {
			if ((payload[i + k] & 0xC0) != 0x80)
				return false;
		}
		i += extra + 1;
	}
	return true;
}

#define VAULT_PREVIEW_BYTES 2048

static esp_err_t vault_handler(httpd_req_t *request)
{
	const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
	std::vector<NightMare::RetainedEntry> entries;
	nightmare_gateway_vault_snapshot(entries);

	cJSON *root = cJSON_CreateObject();
	if (root == nullptr) {
		return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
								   "Unable to allocate JSON response");
	}
	cJSON_AddNumberToObject(root, "uptime_ms", static_cast<double>(now_ms));
	cJSON_AddNumberToObject(root, "count", entries.size());
	cJSON *list = cJSON_AddArrayToObject(root, "messages");

	for (const NightMare::RetainedEntry &entry : entries) {
		const NightMare::Message &message = entry.message;
		const bool truncated = message.payload.size() > VAULT_PREVIEW_BYTES;
		const std::vector<uint8_t> shown(message.payload.begin(),
										 message.payload.begin() + (truncated ? VAULT_PREVIEW_BYTES : message.payload.size()));
		const bool text = is_text(shown);

		cJSON *item = cJSON_CreateObject();
		cJSON_AddStringToObject(item, "topic", message.topic.c_str());
		cJSON_AddStringToObject(item, "origin",
								message.direction == NightMare::Direction::REMOTE_TO_LOCAL ? "remote" : "local");
		cJSON_AddNumberToObject(item, "size", message.payload.size());
		cJSON_AddBoolToObject(item, "truncated", truncated);
		cJSON_AddStringToObject(item, "encoding", text ? "text" : "hex");
		if (text) {
			const std::string body(shown.begin(), shown.end());
			cJSON_AddStringToObject(item, "payload", body.c_str());
		} else {
			static const char digits[] = "0123456789abcdef";
			std::string hex;
			hex.reserve(shown.size() * 2);
			for (uint8_t byte : shown) {
				hex += digits[byte >> 4];
				hex += digits[byte & 0x0F];
			}
			cJSON_AddStringToObject(item, "payload", hex.c_str());
		}
		cJSON_AddNumberToObject(item, "age_ms",
								static_cast<double>(now_ms > entry.updatedAtMs ? now_ms - entry.updatedAtMs : 0));
		cJSON_AddNumberToObject(item, "revisions", entry.revisions);
		cJSON_AddItemToArray(list, item);
	}
	return send_json(request, root);
}

static esp_err_t state_handler(httpd_req_t *request)
{
	const SystemState state = read_system_state();
	const char *gateway_state = nightmare_gateway_state_name(state.gateway_state);
	const bool ready = state.gateway_state == NightMareGatewayState::Running;

	if (request_text_format(request)) {
		char beacon_age[16];
		char temperature[16];
		if (state.seconds_since_last_beacon == UINT32_MAX)
			strcpy(beacon_age, "unavailable");
		else
			snprintf(beacon_age, sizeof(beacon_age), "%lu",
					 static_cast<unsigned long>(state.seconds_since_last_beacon));
		if (state.temperature_available)
			snprintf(temperature, sizeof(temperature), "%.2f", state.temperature_c);
		else
			strcpy(temperature, "unavailable");

		char body[512];
		int length = snprintf(body, sizeof(body),
							  "ready=%s\ngateway_state=%s\nwifi_started=%s\n"
							  "wifi_connected=%s\nmqtt_connected=%s\n"
							  "esp_now_beacon_active=%s\nesp_now_devices=%u\n"
							  "esp_now_subscribers=%u\nseconds_since_last_beacon=%s\n"
							  "internal_temperature_c=%s\n",
							  ready ? "true" : "false", gateway_state,
							  state.wifi_started ? "true" : "false",
							  state.wifi_connected ? "true" : "false",
							  state.mqtt_connected ? "true" : "false",
							  state.beacon_active ? "true" : "false",
							  static_cast<unsigned>(state.device_count),
							  static_cast<unsigned>(state.subscriber_count),
							  beacon_age, temperature);
		httpd_resp_set_type(request, "text/plain; charset=utf-8");
		return httpd_resp_send(request, body, length);
	}

	cJSON *root = cJSON_CreateObject();
	if (root == nullptr) {
		return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
								   "Unable to allocate JSON response");
	}
	cJSON_AddBoolToObject(root, "ready", ready);
	cJSON_AddStringToObject(root, "gateway_state", gateway_state);

	cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
	cJSON_AddBoolToObject(wifi, "started", state.wifi_started);
	cJSON_AddBoolToObject(wifi, "connected", state.wifi_connected);

	cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
	cJSON_AddBoolToObject(mqtt, "connected", state.mqtt_connected);

	cJSON *esp_now = cJSON_AddObjectToObject(root, "esp_now");
	cJSON_AddBoolToObject(esp_now, "beacon_active", state.beacon_active);
	cJSON_AddNumberToObject(esp_now, "devices", state.device_count);
	cJSON_AddNumberToObject(esp_now, "subscribers", state.subscriber_count);
	if (state.seconds_since_last_beacon == UINT32_MAX)
		cJSON_AddNullToObject(esp_now, "seconds_since_last_beacon");
	else
		cJSON_AddNumberToObject(esp_now, "seconds_since_last_beacon", state.seconds_since_last_beacon);

	if (state.temperature_available)
		cJSON_AddNumberToObject(root, "internal_temperature_c", state.temperature_c);
	else
		cJSON_AddNullToObject(root, "internal_temperature_c");
	return send_json(request, root);
}

static esp_err_t metrics_handler(httpd_req_t *request)
{
	uint32_t free_heap = esp_get_free_heap_size();
	uint32_t minimum_free_heap = esp_get_minimum_free_heap_size();
	uint32_t largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
	int64_t uptime_us = esp_timer_get_time();
	float temperature_c = 0.0f;
	const bool temperature_available = chipTempRead(temperature_c);

	if (request_text_format(request)) {
		char temperature[16];
		if (temperature_available)
			snprintf(temperature, sizeof(temperature), "%.2f", temperature_c);
		else
			strcpy(temperature, "unavailable");

		char body[256];
		int length = snprintf(body, sizeof(body),
							  "uptime_seconds=%lld\nfree_heap=%lu\n"
							  "minimum_free_heap=%lu\nlargest_free_block=%lu\n"
							  "internal_temperature_c=%s\n",
							  static_cast<long long>(uptime_us / 1000000),
							  static_cast<unsigned long>(free_heap),
							  static_cast<unsigned long>(minimum_free_heap),
							  static_cast<unsigned long>(largest_free_block),
							  temperature);
		httpd_resp_set_type(request, "text/plain; charset=utf-8");
		return httpd_resp_send(request, body, length);
	}

	cJSON *root = cJSON_CreateObject();
	if (root == nullptr) {
		return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
								   "Unable to allocate JSON response");
	}
	cJSON_AddNumberToObject(root, "uptime_seconds", static_cast<double>(uptime_us) / 1000000.0);
	cJSON_AddNumberToObject(root, "free_heap", free_heap);
	cJSON_AddNumberToObject(root, "minimum_free_heap", minimum_free_heap);
	cJSON_AddNumberToObject(root, "largest_free_block", largest_free_block);
	if (temperature_available)
		cJSON_AddNumberToObject(root, "internal_temperature_c", temperature_c);
	else
		cJSON_AddNullToObject(root, "internal_temperature_c");
	return send_json(request, root);
}

static esp_err_t health_handler(httpd_req_t *request)
{
	const bool ready = nightmare_gateway_state() == NightMareGatewayState::Running;
	if (!ready)
		httpd_resp_set_status(request, "503 Service Unavailable");
	httpd_resp_set_type(request, "text/plain; charset=utf-8");
	return httpd_resp_send(request, ready ? "ok\n" : "starting\n", HTTPD_RESP_USE_STRLEN);
}

esp_err_t start(uint16_t port)
{
	if (s_server != nullptr) {
		return ESP_OK;
	}

	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.server_port = port;
	config.max_uri_handlers = 16;
	config.stack_size = 8192; // cJSON printing recurses
	config.lru_purge_enable = true; // browsers hold keep-alive sockets; never lock out a new tab

	esp_err_t result = httpd_start(&s_server, &config);
	if (result != ESP_OK) {
		return result;
	}

	httpd_uri_t state_uri = {};
	state_uri.uri = "/api/state";
	state_uri.method = HTTP_GET;
	state_uri.handler = state_handler;

	httpd_uri_t metrics_uri = {};
	metrics_uri.uri = "/api/metrics";
	metrics_uri.method = HTTP_GET;
	metrics_uri.handler = metrics_handler;

	httpd_uri_t health_uri = {};
	health_uri.uri = "/health";
	health_uri.method = HTTP_GET;
	health_uri.handler = health_handler;

	httpd_uri_t live_uri = {};
	live_uri.uri = "/api/live";
	live_uri.method = HTTP_GET;
	live_uri.handler = live_handler;

	httpd_uri_t vault_uri = {};
	vault_uri.uri = "/api/vault";
	vault_uri.method = HTTP_GET;
	vault_uri.handler = vault_handler;

	httpd_register_uri_handler(s_server, &live_uri);
	httpd_register_uri_handler(s_server, &vault_uri);
	for (const EmbeddedFile &file : s_files) {
		httpd_uri_t uri = {};
		uri.uri = file.uri;
		uri.method = HTTP_GET;
		uri.handler = file_handler;
		uri.user_ctx = const_cast<EmbeddedFile *>(&file);
		httpd_register_uri_handler(s_server, &uri);
	}
	httpd_register_uri_handler(s_server, &state_uri);
	httpd_register_uri_handler(s_server, &metrics_uri);
	httpd_register_uri_handler(s_server, &health_uri);
	ESP_LOGI(TAG, "HTTP server listening on port %u", port);
	return ESP_OK;
}

void stop()
{
	if (s_server != nullptr) {
		esp_err_t result = httpd_stop(s_server);
		if (result != ESP_OK) {
			ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(result));
		}
		s_server = nullptr;
		ESP_LOGI(TAG, "HTTP server stopped");
	}
}

} // namespace web_server
