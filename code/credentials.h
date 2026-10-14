// credentials.h
// All secrets / account-specific settings for the Dendrometer Logger.
// DO NOT commit this file (add "credentials.h" to .gitignore).

#ifndef CREDENTIALS_H
#define CREDENTIALS_H

// ---------------- WiFi ----------------
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// ---------------- Google service account ----------------
#define PROJECT_ID     ""
#define CLIENT_EMAIL   ""
#define CLIENT_ID      ""
#define PRIVATE_KEY_ID ""

const char PRIVATE_KEY[] PROGMEM = "";

// ---------------- Google Sheet target ----------------
const char spreadsheetId[] = "";
#define SHEET_RANGE ""

#endif // CREDENTIALS_H
