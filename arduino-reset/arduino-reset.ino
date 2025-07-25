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
  { "www.google.com",    "/" },
  { "www.amazon.com",    "/" },
  { "www.microsoft.com", "/" },
  { "www.cloudflare.com","/" }
};
const int numTargets = sizeof(targets) / sizeof(targets[0]);
int       currentTargetIndex = 0; // Tracks which target to ping next

// Digital output pins
#define FAILURE_PIN 8  // This pin goes HIGH when connection is considered lost.
#define RECOVERY_PIN 9 // This pin pulses HIGH when connection is restored.

// Timing configuration (in milliseconds)
#define PING_INTERVAL             30000  // How often to attempt a ping: 30 seconds.
#define FAILURE_THRESHOLD         300000 // Mark as failed after 5 minutes of no successful pings.
#define FAILURE_SIGNAL_DURATION   60000  // How long the failure pin stays HIGH: 1 minute.
#define FAILURE_COOLDOWN_DURATION 300000 // How long to wait after failure before re-triggering.
#define RECOVERY_PULSE_DURATION   5000   // Duration of the recovery signal pulse: 5 seconds.
#define HTTP_TIMEOUT              10000  // Max time to wait for HTTP connection/response: 10 seconds.


// =================================================================
// --- SYSTEM STATE & GLOBALS (DO NOT MODIFY) ---
// =================================================================

EthernetClient client;

// --- State Management Enums ---
// This enum defines the overall state of the connection monitor.
enum SystemState {
  STATE_NORMAL,         // Everything is fine, pinging periodically.
  STATE_FAILED_SIGNAL,  // Connection lost, failure pin is HIGH.
  STATE_COOLDOWN        // Failure signal ended, actively checking for recovery.
};
SystemState currentState = STATE_NORMAL;

// This enum manages the states of the non-blocking ping process.
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
unsigned long pingStateStartTime           = 0; // Timer for the current ping state (e.g., for timeouts)

// --- Flags ---
bool recoveryPulseActive = false;

// =================================================================
// --- SETUP ---
// =================================================================

void setup() {
  // Initialize digital pins for output and set them low initially
  pinMode(FAILURE_PIN, OUTPUT);
  pinMode(RECOVERY_PIN, OUTPUT);
  digitalWrite(FAILURE_PIN, LOW);
  digitalWrite(RECOVERY_PIN, LOW);

  // Open serial communications and wait for port to open
  Serial.begin(9600);
  while (!Serial) {
    ; // wait for serial port to connect. Needed for native USB port only
  }
  Serial.println(F("\n\nInternet Connection Monitor (Non-Blocking Version)"));
  Serial.println(F("=============================================="));

  // Start the Ethernet connection, retrying if it fails.
  Serial.println(F("Initializing Ethernet with DHCP..."));
  if (Ethernet.begin(mac) == 0) {
    Serial.println(F("FATAL: Failed to configure Ethernet using DHCP."));
    // If DHCP fails, we can't do anything. The program will hang here.
    // Consider adding a fallback to a static IP if this is a concern.
    while (true) { delay(1000); }
  }

  // Print the local IP address obtained from DHCP
  Serial.print(F("DHCP successful. IP address: "));
  Serial.println(Ethernet.localIP());
  delay(1000); // Give the shield a second to initialize fully

  // Initialize timers. This correctly handles the startup period.
  lastSuccessfulConnectionTime = millis();
  pingStateStartTime = millis() - PING_INTERVAL; // Set to trigger the first ping immediately.
}


// =================================================================
// --- MAIN LOOP ---
// =================================================================

void loop() {
  // Task 1: Always maintain the DHCP lease.
  Ethernet.maintain();

  // Task 2: Manage the overall system state (Normal -> Failed -> Cooldown).
  manageSystemState();

  // Task 3: Manage the non-blocking ping process state machine.
  // This function handles everything from connecting to reading the response
  // without blocking the main loop.
  managePingProcess();

  // Task 4: Manage the recovery pin pulse timer.
  manageRecoveryPulse();
}


// =================================================================
// --- STATE MANAGEMENT FUNCTIONS ---
// =================================================================

/**
 * @brief Manages transitions between NORMAL, FAILED_SIGNAL, and COOLDOWN states.
 */
void manageSystemState() {
  unsigned long now = millis();

  switch (currentState) {
    case STATE_NORMAL:
      // Check if the failure threshold has been exceeded.
      if (now - lastSuccessfulConnectionTime >= FAILURE_THRESHOLD) {
        // --- Transition to FAILED_SIGNAL state ---
        currentState = STATE_FAILED_SIGNAL;
        failureStateStartTime = now;
        digitalWrite(FAILURE_PIN, HIGH);
        Serial.println(F("\n!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"));
        Serial.println(F("!!      CONNECTION STATE: FAILED      !!"));
        Serial.print(F("!! Failure pin HIGH for "));
        Serial.print(FAILURE_SIGNAL_DURATION / 1000);
        Serial.println(F(" seconds."));
        Serial.println(F("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n"));
      }
      break;

    case STATE_FAILED_SIGNAL:
      // Check if the failure signal duration has ended.
      if (now - failureStateStartTime >= FAILURE_SIGNAL_DURATION) {
        // --- Transition to COOLDOWN state ---
        currentState = STATE_COOLDOWN;
        cooldownStateStartTime = now;
        digitalWrite(FAILURE_PIN, LOW);
        Serial.println(F("\n----------------------------------------"));
        Serial.println(F("-> Failure signal finished."));
        Serial.print(F("-> Entering COOLDOWN. Actively checking for recovery for "));
        Serial.print(FAILURE_COOLDOWN_DURATION / 1000);
        Serial.println(F(" seconds."));
        Serial.println(F("----------------------------------------\n"));
      }
      break;

    case STATE_COOLDOWN:
      // Check if the cooldown period has timed out without a recovery.
      if (now - cooldownStateStartTime >= FAILURE_COOLDOWN_DURATION) {
        // --- Transition back to NORMAL state to re-evaluate ---
        currentState = STATE_NORMAL;
        // The last successful connection time is still old, so the system
        // will likely re-enter the FAILED state on the next loop, which is correct.
        Serial.println(F("\n****************************************"));
        Serial.println(F("** Cooldown timeout. Re-evaluating... **"));
        Serial.println(F("****************************************\n"));
      }
      break;
  }
}

/**
 * @brief Manages the non-blocking ping state machine. This is the core of the sketch.
 */
void managePingProcess() {
  unsigned long now = millis();
  const Target& currentTarget = targets[currentTargetIndex];

  switch (pingState) {
    case PING_IDLE:
      // In IDLE state, we wait for the ping interval to pass.
      // We do not ping if the system is in the middle of a hard failure signal.
      if (currentState != STATE_FAILED_SIGNAL && (now - pingStateStartTime >= PING_INTERVAL)) {
        Serial.println(F("----------------------------------------"));
        Serial.print(F("Pinging http://"));
        Serial.print(currentTarget.server);
        Serial.println(currentTarget.path);

        // --- Transition to CONNECTING state ---
        pingState = PING_CONNECTING;
        pingStateStartTime = now; // Start the timer for this attempt.
      }
      break;

    case PING_CONNECTING:
      // Attempt to connect. This is non-blocking.
      if (client.connect(currentTarget.server, 80)) {
        Serial.println(F("-> Connection to server established."));
        // --- Transition to SENDING state ---
        pingState = PING_SENDING;
        pingStateStartTime = now;
      }
      // Check for connection timeout.
      else if (now - pingStateStartTime >= HTTP_TIMEOUT) {
        Serial.println(F("-> Failure: Connection attempt timed out."));
        client.stop();
        // --- Transition to DONE state ---
        pingState = PING_DONE;
        pingStateStartTime = now;
      }
      // Otherwise, we just return and try again on the next loop.
      break;

    case PING_SENDING:
      // Send the HTTP request headers.
      client.print("GET "); client.print(currentTarget.path); client.println(" HTTP/1.1");
      client.print("Host: "); client.println(currentTarget.server);
      client.println("Connection: close");
      client.println();
      // --- Transition to READING state ---
      pingState = PING_READING;
      pingStateStartTime = now;
      break;

    case PING_READING:
      // Wait for the server to send a response.
      if (client.available()) {
        // Use a char buffer to avoid the String class, which can cause memory issues.
        char statusLine[90];
        // Read just the first line of the response.
        int bytesRead = client.readBytesUntil('\n', statusLine, sizeof(statusLine) - 1);
        statusLine[bytesRead] = '\0'; // Null-terminate the C-string.

        Serial.print(F("-> Server response: ")); Serial.println(statusLine);

        // A 2xx or 3xx response indicates a working connection.
        // We check for "HTTP/1.1 2" or "HTTP/1.1 3". This is robust.
        if (strstr(statusLine, "HTTP/1.1 2") != NULL || strstr(statusLine, "HTTP/1.1 3") != NULL) {
          handleSuccess(now - (pingStateStartTime - (pingStateStartTime - cooldownStateStartTime))); // Pass total request time
        } else {
          Serial.println(F("-> Failure: Received a non-2xx/3xx status code."));
        }
        // --- Transition to DONE state ---
        pingState = PING_DONE;
        pingStateStartTime = now;
      }
      // Check for response timeout.
      else if (now - pingStateStartTime >= HTTP_TIMEOUT) {
        Serial.println(F("-> Failure: HTTP response timeout."));
        // --- Transition to DONE state ---
        pingState = PING_DONE;
        pingStateStartTime = now;
      }
      break;

    case PING_DONE:
      // Clean up the connection and cycle to the next target.
      client.stop();
      Serial.println(F("-> Connection closed."));
      currentTargetIndex = (currentTargetIndex + 1) % numTargets;
      // --- Transition back to IDLE state ---
      pingState = PING_IDLE;
      pingStateStartTime = now; // Reset the ping interval timer.
      break;
  }
}

/**
 * @brief Manages the timer for the recovery pulse signal.
 */
void manageRecoveryPulse() {
  if (recoveryPulseActive && (millis() - recoveryPulseStartTime >= RECOVERY_PULSE_DURATION)) {
    recoveryPulseActive = false;
    digitalWrite(RECOVERY_PIN, LOW);
    Serial.println(F("-> Recovery pulse finished."));
  }
}

// =================================================================
// --- HELPER FUNCTIONS ---
// =================================================================

/**
 * @brief Handles the logic for a successful connection.
 * @param duration The time taken for the successful ping in milliseconds.
 */
void handleSuccess(unsigned long duration) {
  Serial.print(F("-> SUCCESS! Response time: "));
  Serial.print(duration);
  Serial.println(F(" ms"));

  // A successful ping always updates the last success time.
  unsigned long now = millis();
  lastSuccessfulConnectionTime = now;

  // If this success happened while we were in a failure/cooldown cycle, it's a recovery.
  if (currentState == STATE_FAILED_SIGNAL || currentState == STATE_COOLDOWN) {
    // Calculate outage duration from the moment the failure process began.
    unsigned long outageDuration = now - failureStateStartTime;

    // --- RECOVERY: Reset all state flags back to normal ---
    currentState = STATE_NORMAL;
    digitalWrite(FAILURE_PIN, LOW); // Ensure failure pin is off

    Serial.println(F("\n****************************************"));
    Serial.println(F("** CONNECTION STATE: RECOVERED     **"));
    Serial.print(F("** Outage lasted for: "));
    Serial.print(outageDuration / 1000.0, 2);
    Serial.println(F(" seconds."));
    Serial.println(F("****************************************\n"));

    // Activate the recovery signal pulse
    recoveryPulseActive = true;
    digitalWrite(RECOVERY_PIN, HIGH);
    recoveryPulseStartTime = now;
    Serial.print(F("-> Starting recovery pulse for "));
    Serial.print(RECOVERY_PULSE_DURATION / 1000);
    Serial.println(F(" seconds."));
  }
}
