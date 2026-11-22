// Copy this file to "birthdays.h" (same folder) and edit it. birthdays.h is listed in .gitignore,
// so the real names and dates never get uploaded.
//
// On a listed date the frame shows the message full screen (waving rainbow letters, floating hearts
// and sparkles) at power-up and then every 15 minutes on the quarter hour.
//
// Format: { month, day, "message" }. Plain ASCII only: the display fonts have no emoji or accents.
// Each message is word-wrapped to at most 3 lines of 31 characters.
// Needs at least one entry; use a date that never comes up if you want the feature effectively off,
// for example { 2, 30, "unused" }.
#pragma once

const Birthday BIRTHDAYS[] = {
  { 1,  1, "Happy Birthday, Alex!!"},
  { 6, 15, "Happy Birthday, Sam!!"},
};
