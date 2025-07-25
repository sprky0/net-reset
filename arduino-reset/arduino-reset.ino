/*
  Internet Connection Monitor for Arduino Uno + Ethernet Shield (Multi-Target)
*/

#include <SPI.h>
#include <Ethernet.h>

// =================================================================
// --- USER CONFIGURATION ---
// =================================================================

// MAC address for the Ethernet shield. This must be unique on your network.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// Define a structure to hold our target server and path
struct Target {
  const char* server;
  const char* path;
};

// Array of targets to ping. Add or remove targets as needed.
const Target targets[] = {
  { "www.google.com",    "/" },
  { "www.amazon.com",    "/" },
  { "www.microsoft.com", "/" },
  { "www.cloudflare.com","/" },
  { "www.bbc.co.uk",     "/" } // Example European server
};
const int numTargets = sizeof(targets) / sizeof(targets[0]);
int currentTargetIndex = 0; // Tracks which target to ping next

// Digital output pins
const int failurePin = 8;    // This pin goes HIGH when connection is considered lost.
const int recoveryPin = 9;   // This pin pulses HIGH when connection is restored.

// Timing configuration (in milliseconds)
const unsigned long pingInterval = 30000;          // How often to ping: 30 seconds.
const unsigned long failureThreshold = 300000;     // Mark as failed after 5 minutes of no connection.
const unsigned long recoveryPulseDuration = 5000;  // Duration of the recovery signal pulse: 5 seconds.
const unsigned long httpTimeout = 10000;           // Max time to wait for HTTP response: 10 seconds.

// =================================================================
// --- GLOBAL VARIABLES (DO NOT MODIFY) ---
// =================================================================

EthernetClient client;

// State tracking
bool isFailed = false;
bool recoveryPulseActive = false;

// Timers
unsigned long lastAttemptTime = 0;
unsigned long lastSuccessfulConnectionTime = 0;
unsigned long failureStartTime = 0;
unsigned long recoveryPulseStartTime = 0;

// Statistics for successful connections
unsigned long totalSuccessDuration = 0;
unsigned long successCount = 0;
float averageSuccessDuration = 0.0;

// Statistics for failures
unsigned long totalFailureDuration = 0;
unsigned long failureCount = 0;
float averageFailureDuration = 0.0;


// =================================================================
// --- SETUP ---
// =================================================================

void setup() {
	// Initialize digital pins for output and set them low initially
	pinMode(failurePin, OUTPUT);
	pinMode(recoveryPin, OUTPUT);
	digitalWrite(failurePin, LOW);
	digitalWrite(recoveryPin, LOW);

	// Open serial communications and wait for port to open:
	Serial.begin(9600);
	while (!Serial) {
		; // wait for serial port to connect. Needed for native USB port only
	}
	Serial.println("Internet Connection Monitor (Multi-Target)");
	Serial.println("==========================================");

	// Start the Ethernet connection:
	Serial.println("Initializing Ethernet with DHCP...");
	while (Ethernet.begin(mac) == 0) {
		Serial.println("Failed to configure Ethernet using DHCP");
		// If DHCP fails at startup, it's a critical error.
		// digitalWrite(failurePin, HIGH);
		Serial.println("Ethernet failed to initialize. Trying again in 5 seconds...");
		delay(5000); // Wait before retrying
	}

	// Print the local IP address obtained from DHCP
	Serial.print("DHCP successful. IP address: ");
	Serial.println(Ethernet.localIP());
	delay(1000); // Give the shield a second to initialize fully

	// Initialize timers. This correctly handles the startup period.
	lastAttemptTime = millis();
	lastSuccessfulConnectionTime = millis();
}

// =================================================================
// --- MAIN LOOP ---
// =================================================================

void loop() {
	// --- Task 0: Maintain DHCP lease and network connection ---
	Ethernet.maintain();

	// --- Task 1: Check if it's time to ping the next URL in the list ---
	if (millis() - lastAttemptTime >= pingInterval) {
		// Get the current target from our array
		const Target& currentTarget = targets[currentTargetIndex];
		
		// Ping the URL
		pingURL(currentTarget.server, currentTarget.path);
		
		// Move to the next target for the next cycle
		currentTargetIndex = (currentTargetIndex + 1) % numTargets;
		
		lastAttemptTime = millis(); // Reset the attempt timer
	}

	// --- Task 2: Check if the connection has entered a "failed" state ---
	if (!isFailed && (millis() - lastSuccessfulConnectionTime >= failureThreshold)) {
		isFailed = true;
		failureStartTime = millis(); // Record when the failure began
		digitalWrite(failurePin, HIGH);
		Serial.println("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		Serial.println("!!      CONNECTION STATE: FAILED      !!");
		Serial.print("!! No successful connection for > ");
		Serial.print(failureThreshold / 1000);
		Serial.println(" seconds.");
		Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
	}

	// --- Task 3: Manage the recovery pin pulse ---
	if (recoveryPulseActive && (millis() - recoveryPulseStartTime >= recoveryPulseDuration)) {
		recoveryPulseActive = false;
		digitalWrite(recoveryPin, LOW); // Turn off the recovery pin
		Serial.println("-> Recovery pulse finished.");
	}
}

// =================================================================
// --- HELPER FUNCTIONS ---
// =================================================================

/**
 * @brief Performs the HTTP GET request and updates system state.
 * @param server The server hostname to connect to.
 * @param path The path for the GET request.
 */
void pingURL(const char* server, const char* path) {
	Serial.println("----------------------------------------");
	Serial.print("Pinging http://");
	Serial.print(server);
	Serial.println(path);

unsigned long requestStartTime = millis();

if (client.connect(server, 80)) {
	Serial.println("-> Connection to server established.");
	
	// Make an HTTP request:
	client.print("GET ");
	client.print(path);
	client.println(" HTTP/1.1");
	client.print("Host: ");
	client.println(server);
	client.println("Connection: close");
	client.println();

	unsigned long timeoutStart = millis();
	while (!client.available() && (millis() - timeoutStart < httpTimeout)) {
	// Wait for data to become available
	}

	if (client.available()) {
		String statusLine = client.readStringUntil('\n');
		statusLine.trim();
		Serial.print("-> Server response: ");
		Serial.println(statusLine);

		if (statusLine.indexOf("200 OK") != -1) {
			unsigned long requestDuration = millis() - requestStartTime;
			handleSuccess(requestDuration);
		} else {
			Serial.println("-> Failure: Received a non-200 status code.");
		}

		} else {
		Serial.println("-> Failure: HTTP response timeout.");
		}
		
	} else {
		Serial.println("-> Failure: Could not connect to server.");
	}

	client.stop();
	Serial.println("-> Connection closed.");
}

/**
 * @brief Handles the logic for a successful connection.
 * @param duration The time it took for the successful request in milliseconds.
 */
void handleSuccess(unsigned long duration) {
	Serial.print("-> SUCCESS! Response time: ");
	Serial.print(duration);
	Serial.println(" ms");

	// Update statistics for successful connections
	successCount++;
	totalSuccessDuration += duration;
	averageSuccessDuration = (float)totalSuccessDuration / successCount;
	Serial.print("-> Average success time (all targets): ");
	Serial.print(averageSuccessDuration, 2);
	Serial.println(" ms");

	// Update the time of the last successful connection
	lastSuccessfulConnectionTime = millis();

	// Check if we are recovering from a failed state
	if (isFailed) {
		isFailed = false;
		digitalWrite(failurePin, LOW); // Turn off the failure indicator

		unsigned long outageDuration = millis() - failureStartTime;
		
		failureCount++;
		totalFailureDuration += outageDuration;
		averageFailureDuration = (float)totalFailureDuration / failureCount;

		Serial.println("\n****************************************");
		Serial.println("** CONNECTION STATE: RECOVERED     **");
		Serial.print("** Outage lasted for: ");
		Serial.print(outageDuration / 1000.0, 2);
		Serial.println(" seconds.");
		Serial.print("** Average outage duration: ");
		Serial.print(averageFailureDuration / 1000.0, 2);
		Serial.println(" seconds.");
		Serial.println("****************************************\n");
		
		// Activate the recovery pulse
		recoveryPulseActive = true;
		digitalWrite(recoveryPin, HIGH);
		recoveryPulseStartTime = millis();
		Serial.print("-> Starting recovery pulse for ");
		Serial.print(recoveryPulseDuration / 1000);
		Serial.println(" seconds.");
	}
}
