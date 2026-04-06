#include "storage_prefs.h"

#include <Preferences.h>
#include "app_state.h"

static Preferences prefs;

void initStoragePrefs() {
  prefs.begin("keyboard", false);
}

String getStageKey(int stage) {
  if (stage == 0) return "wifi_ssid";
  if (stage == 1) return "wifi_pass";
  return "server_ip";
}

String getStageTitle(int stage) {
  if (stage == 0) return "WiFi Name:";
  if (stage == 1) return "WiFi Password:";
  return "Server IP:";
}

void loadSavedConfig() {
  if (prefs.getString("wifi_ssid", "") == "") {
    prefs.putString("wifi_ssid", "Galaxy M12 C9A6");
  }

  if (prefs.getString("wifi_pass", "") == "") {
    prefs.putString("wifi_pass", "zhof1469");
  }

  if (prefs.getString("server_ip", "") == "") {
    prefs.putString("server_ip", "10.19.187.13");
  }

  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  serverIP = prefs.getString("server_ip", "");
}

void saveCurrentStageValue() {
  prefs.putString(getStageKey(CURRENT_STAGE).c_str(), inputText);

  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPASS = prefs.getString("wifi_pass", "");
  serverIP = prefs.getString("server_ip", "");
}

String loadStageValue(int stage) {
  return prefs.getString(getStageKey(stage).c_str(), "");
}