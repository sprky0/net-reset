// =================================================================
// --- Internet Connection Monitor (Non-Blocking Version) ---
// =================================================================

#include <SPI.h>
#include <Ethernet.h>

// =================================================================
// --- USER CONFIGURATION ---
// =================================================================

// MAC address for the Ethernet shield. This MUST be unique on your local network.
// You can often find a MAC address printed on a sticker on the shield itself.
// Do not use the same MAC address for multiple devices on your network.
byte mac[] = { 0x0A, 0x01, 0x0A, 0x01, 0x03, 0x02 };

// Define a structure to hold our target server and path
struct Target {
	const char* server;
	const char* path;
};

// Array of targets to ping. Add or remove targets as needed.
const Target targets[] = {
	{ "www.google.com",       "/" },
	{ "www.amazon.com",       "/" },
	{ "www.microsoft.com",    "/" },
	{ "www.cloudflare.com",   "/" },
	{ "www.apple.com",        "/" },
	{ "www.facebook.com",     "/" },
	{ "www.youtube.com",      "/" },
	{ "www.twitter.com",      "/" },
	{ "www.linkedin.com",     "/" },
	{ "www.github.com",       "/" },
	{ "www.stackoverflow.com","/" },
	{ "www.reddit.com",       "/" },
	{ "www.wikipedia.org",    "/" },
	{ "www.cnn.com",          "/" },
	{ "www.bbc.com",          "/" },
	{ "www.yahoo.com",        "/" },
	{ "www.bing.com",         "/" },
	{ "www.netflix.com",      "/" },
	{ "www.adobe.com",        "/" },
	{ "www.salesforce.com",   "/" },
	{ "www.oracle.com",       "/" },
	{ "www.ibm.com",          "/" },
	{ "www.cisco.com",        "/" },
	{ "www.intel.com",        "/" },
	{ "www.hp.com",           "/" },
	{ "www.dell.com",         "/" },
	{ "www.ubuntu.com",       "/" },
	{ "www.mozilla.org",      "/" },
	{ "www.w3.org",           "/" },
	{ "httpbin.org",          "/" }
};
const int numTargets = sizeof(targets) / sizeof(targets[0]);
int       currentTargetIndex = 0; // Tracks which target to ping next

// Digital output pins
#define PINGING_PIN         7 // This pin pulses HIGH briefly when making a ping request.
#define FAILURE_STATE_PIN   8  // This pin is HIGH whenever not in STATE_NORMAL (failure or cooldown).
#define FAILURE_SIGNAL_PIN  9  // This pin goes HIGH for a timed duration when connection is considered lost.
#define RECOVERY_SIGNAL_PIN 10 // This pin pulses HIGH when connection is restored.

// Timing configuration (in milliseconds)
#define PING_INTERVAL             10000  // How often to attempt a ping: 10 seconds.
#define FAILURE_THRESHOLD         300000 // Mark as failed after 5 minutes of no successful pings.
#define FAILURE_SIGNAL_DURATION   60000  // How long the failure signal pin stays HIGH: 1 minute.
#define FAILURE_COOLDOWN_DURATION 300000 // How long to wait after failure before re-triggering.
#define RECOVERY_PULSE_DURATION   5000   // Duration of the recovery signal pulse: 5 seconds.
#define PINGING_PULSE_DURATION    500   // Duration of the pinging signal pulse: 500 milliseconds.
#define HTTP_TIMEOUT              10000  // Max time to wait for HTTP connection/response: 10 seconds.


// =================================================================
// --- SYSTEM STATE & GLOBALS (DO NOT MODIFY) ---
// =================================================================

EthernetClient client;

// --- State Management Enums ---
enum SystemState {
	STATE_NORMAL,         // Everything is fine, pinging periodically.
	STATE_FAILED_SIGNAL,  // Connection lost, failure signal pin is HIGH.
	STATE_COOLDOWN        // Failure signal ended, actively checking for recovery.
};
SystemState currentState = STATE_NORMAL;

enum PingState {
	PING_IDLE,            // Waiting for the ping interval to elapse.
	PING_CONNECTING,      // Attempting to connect to the server.
	PING_SENDING,         // Sending the HTTP GET request.
	PING_READING,         // Waiting for and reading the HTTP response.
	PING_DONE             // Cleaning up after a ping attempt (success or fail).
};
PingState pingState = PING_IDLE;

// --- Timers ---
unsigned long lastSuccessfulConnectionTime = 0;
unsigned long failureStateStartTime        = 0;
unsigned long cooldownStateStartTime       = 0;
unsigned long recoveryPulseStartTime       = 0;
unsigned long pingingPulseStartTime        = 0;
unsigned long pingStateStartTime           = 0; // Timer for the current ping sub-state (e.g., for timeouts)
unsigned long pingTransactionStartTime     = 0; // Timer for the entire ping transaction duration

// --- Flags ---
bool recoveryPulseActive = false;
bool pingingPulseActive  = false;

// =================================================================
// --- SETUP ---
// =================================================================

void setup() {
	pinMode(FAILURE_STATE_PIN, OUTPUT);
	pinMode(FAILURE_SIGNAL_PIN, OUTPUT);
	pinMode(RECOVERY_SIGNAL_PIN, OUTPUT);
	pinMode(PINGING_PIN, OUTPUT);
	digitalWrite(FAILURE_STATE_PIN, LOW);
	digitalWrite(FAILURE_SIGNAL_PIN, LOW);
	digitalWrite(RECOVERY_SIGNAL_PIN, LOW);
	digitalWrite(PINGING_PIN, LOW);

	Serial.begin(9600);
	delay(1000);
	Serial.println(F("\n\nInternet Connection Monitor (Non-Blocking Version)"));
	Serial.println(F("=============================================="));

	Serial.println(F("Initializing Ethernet with DHCP..."));
	int dhcpRetries = 0;
	const int MAX_DHCP_RETRIES = 10;
	while (Ethernet.begin(mac) == 0 && dhcpRetries < MAX_DHCP_RETRIES) {
		dhcpRetries++;
		Serial.print(F("Failed to configure Ethernet using DHCP. Retry "));
		Serial.print(dhcpRetries);
		Serial.print(F("/"));
		Serial.print(MAX_DHCP_RETRIES);
		Serial.println(F(" in 5 seconds."));
		delay(5000);
	}
	
	if (dhcpRetries >= MAX_DHCP_RETRIES) {
		Serial.println(F("DHCP failed after maximum retries."));
		Serial.println(F("ERROR: Cannot obtain network configuration automatically."));
		Serial.println(F("Please check network connection and DHCP server."));
		Serial.println(F("System will continue attempting DHCP renewal periodically."));
	} else {
		Serial.print(F("DHCP successful. IP address: "));
		Serial.println(Ethernet.localIP());
	}
	delay(1000);

	lastSuccessfulConnectionTime = millis();
	pingStateStartTime = millis() - PING_INTERVAL;
}

// =================================================================
// --- MAIN LOOP ---
// =================================================================

void loop() {
	Ethernet.maintain();
	manageSystemState();
	managePingProcess();
	managePulseStates();
}

// =================================================================
// --- STATE MANAGEMENT FUNCTIONS ---
// =================================================================

void manageSystemState() {
	unsigned long now = millis();

	switch (currentState) {
		case STATE_NORMAL:
			if (hasTimeElapsed(lastSuccessfulConnectionTime, FAILURE_THRESHOLD)) {
				currentState = STATE_FAILED_SIGNAL;
				failureStateStartTime = now;
				digitalWrite(FAILURE_STATE_PIN, HIGH);
				digitalWrite(FAILURE_SIGNAL_PIN, HIGH);
				Serial.println(F("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
				Serial.println(F("!!      CONNECTION STATE: FAILED      !!"));
				Serial.print(F("!! Failure signal pin HIGH for "));
				Serial.print(FAILURE_SIGNAL_DURATION / 1000);
				Serial.println(F(" seconds."));
				Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"));
			}
			break;

		case STATE_FAILED_SIGNAL:
			if (hasTimeElapsed(failureStateStartTime, FAILURE_SIGNAL_DURATION)) {
				currentState = STATE_COOLDOWN;
				cooldownStateStartTime = now;
				digitalWrite(FAILURE_SIGNAL_PIN, LOW);
				Serial.println(F("\n----------------------------------------"));
				Serial.println(F("-> Failure signal finished."));
				Serial.print(F("-> Entering COOLDOWN. Actively checking for recovery for "));
				Serial.print(FAILURE_COOLDOWN_DURATION / 1000);
				Serial.println(F(" seconds."));
				Serial.println(F("----------------------------------------\n"));
			}
			break;

		case STATE_COOLDOWN:
			if (hasTimeElapsed(cooldownStateStartTime, FAILURE_COOLDOWN_DURATION)) {
				currentState = STATE_NORMAL;
				digitalWrite(FAILURE_STATE_PIN, LOW);
				Serial.println(F("\n****************************************"));
				Serial.println(F("** Cooldown timeout. Re-evaluating... **"));
				Serial.println(F("****************************************\n"));
			}
			break;
	}
}

void managePingProcess() {
	unsigned long now = millis();
	const Target& currentTarget = targets[currentTargetIndex];

	switch (pingState) {
		case PING_IDLE:
			if (currentState != STATE_FAILED_SIGNAL && hasTimeElapsed(pingStateStartTime, PING_INTERVAL)) {
				Serial.println(F("----------------------------------------"));
				Serial.print(F("Pinging http://"));
				Serial.print(currentTarget.server);
				Serial.println(currentTarget.path);

				// Start the pinging signal pulse (set flag, not pin)
				pingingPulseActive = true;
				pingingPulseStartTime = now;
				Serial.println(F("[PING_IDLE → PING_CONNECTING]"));

				pingState = PING_CONNECTING;
				pingStateStartTime = now;
				pingTransactionStartTime = now;
			}
			break;

		case PING_CONNECTING:
			if (client.connect(currentTarget.server, 80)) {
				Serial.println(F("-> Connection to server established."));
				pingState = PING_SENDING;
				pingStateStartTime = now;
				Serial.println(F("[PING_CONNECTING → PING_SENDING]"));
			}
			else if (hasTimeElapsed(pingStateStartTime, HTTP_TIMEOUT)) {
				Serial.println(F("-> Failure: Connection attempt timed out."));
				client.stop();
				pingState = PING_DONE;
				pingStateStartTime = now;
				Serial.println(F("[PING_CONNECTING → PING_DONE]"));
			}
			break;

		case PING_SENDING:
			client.print("GET "); client.print(currentTarget.path); client.println(" HTTP/1.1");
			client.print("Host: "); client.println(currentTarget.server);
			client.println("Connection: close");
			client.println();
			pingState = PING_READING;
			pingStateStartTime = now;
			Serial.println(F("[PING_SENDING → PING_READING]"));
			break;

		case PING_READING:
			if (client.available()) {
				char statusLine[90];
				int bytesRead = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
				statusLine[bytesRead] = '\0';

				Serial.print(F("-> Server response: ")); Serial.println(statusLine);

				if (strstr(statusLine, "HTTP/1.1 2") != NULL || strstr(statusLine, "HTTP/1.1 3") != NULL) {
					handleSuccess(safeTimeDiff(now, pingTransactionStartTime));
				} else {
					Serial.println(F("-> Failure: Received a non-2xx/3xx status code."));
				}
				pingState = PING_DONE;
				pingStateStartTime = now;
				Serial.println(F("[PING_READING → PING_DONE]"));
			}
			else if (hasTimeElapsed(pingStateStartTime, HTTP_TIMEOUT)) {
				Serial.println(F("-> Failure: HTTP response timeout."));
				pingState = PING_DONE;
				pingStateStartTime = now;
				Serial.println(F("[PING_READING → PING_DONE]"));
			}
			break;

		case PING_DONE:
			client.stop();
			Serial.println(F("-> Connection closed."));
			currentTargetIndex = (currentTargetIndex + 1) % numTargets;

			// End the pinging pulse (set flag, not pin)
			pingingPulseActive = false;

			Serial.println(F("-> Pinging pulse ended (transaction complete)."));

			pingState = PING_IDLE;
			pingStateStartTime = now;
			break;
	}
}

void managePulseStates() {
	unsigned long now = millis();

	// PINGING_PIN logic
	if (pingingPulseActive) {
		if ((now - pingingPulseStartTime) < PINGING_PULSE_DURATION) {
			// If we are in the PINGING state, pulse the PINGING_PIN HIGH
			Serial.println(F("-> Setting pinging pulse active."));
			digitalWrite(PINGING_PIN, HIGH);
		} else {
			Serial.println(F("-> Pinging pulse finished / setting pin LOW"));
			digitalWrite(PINGING_PIN, LOW);
			pingingPulseActive = false;
		}
	}
	// else {
	// 	Serial.println(F("-> Pinging pulse inactive, setting pin LOW."));
	// 	digitalWrite(PINGING_PIN, LOW);
	// }

	// RECOVERY_SIGNAL_PIN logic
	if (recoveryPulseActive) {
		if ((now - recoveryPulseStartTime) < RECOVERY_PULSE_DURATION) {
			digitalWrite(RECOVERY_SIGNAL_PIN, HIGH);
		} else {
			digitalWrite(RECOVERY_SIGNAL_PIN, LOW);
			recoveryPulseActive = false;
			Serial.println(F("-> Recovery pulse finished."));
		}
	}
	// else {
	// 	digitalWrite(RECOVERY_SIGNAL_PIN, LOW);
	// }
}

// =================================================================
// --- HELPER FUNCTIONS ---
// =================================================================

unsigned long safeTimeDiff(unsigned long later, unsigned long earlier) {
	return later - earlier;
}

bool hasTimeElapsed(unsigned long startTime, unsigned long interval) {
	return (millis() - startTime) >= interval;
}

void handleSuccess(unsigned long duration) {
	Serial.print(F("-> SUCCESS! Response time: "));
	Serial.print(duration);
	Serial.println(F(" ms"));

	unsigned long now = millis();
	
	if (currentState == STATE_FAILED_SIGNAL || currentState == STATE_COOLDOWN) {
		unsigned long outageDuration = safeTimeDiff(now, lastSuccessfulConnectionTime);

		currentState = STATE_NORMAL;
		digitalWrite(FAILURE_STATE_PIN, LOW);
		digitalWrite(FAILURE_SIGNAL_PIN, LOW);

		Serial.println(F("\n****************************************"));
		Serial.println(F("** CONNECTION STATE: RECOVERED     **"));
		Serial.print(F("** Outage lasted for: "));
		Serial.print(outageDuration / 1000.0, 2);
		Serial.println(F(" seconds."));
		Serial.println(F("****************************************\n"));

		// Activate the recovery signal pulse (set flag, not pin)
		recoveryPulseActive = true;
		recoveryPulseStartTime = now;
		Serial.print(F("-> Starting recovery pulse for "));
		Serial.print(RECOVERY_PULSE_DURATION / 1000);
		Serial.println(F(" seconds."));
	}
	
	lastSuccessfulConnectionTime = now;
}
