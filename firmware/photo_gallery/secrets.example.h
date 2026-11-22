// Copy this file to "secrets.h" (same folder) and fill it in. secrets.h is listed in .gitignore,
// so your Wi-Fi password and home location never get uploaded.
//
// Wi-Fi is used only to set the clock (NTP) and fetch the weather (Open-Meteo, no account needed).
// Without Wi-Fi the frame still shows photos, but there is no clock, date or weather.
#pragma once

// 2.4 GHz networks only (the ESP32-S3 has no 5 GHz radio). A phone hotspot works too.
const char *WIFI_1_SSID = "your-network-name";
const char *WIFI_1_PASS = "your-password";
const char *WIFI_2_SSID = "";    // optional second network (leave "" to skip)
const char *WIFI_2_PASS = "";

// POSIX time-zone string with automatic daylight saving. Examples:
//   Eastern Canada/US: "EST5EDT,M3.2.0,M11.1.0"     UK: "GMT0BST,M3.5.0/1,M10.5.0"
//   Central Europe:    "CET-1CEST,M3.5.0,M10.5.0/3"
const char *TIMEZONE = "EST5EDT,M3.2.0,M11.1.0";

// Where the weather is fetched for (decimal degrees). Example values: Toronto.
const float LATITUDE  = 43.6532;
const float LONGITUDE = -79.3832;
