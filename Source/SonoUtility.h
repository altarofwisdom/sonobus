// SPDX-License-Identifier: GPLv3-or-later WITH Appstore-exception
// Copyright (C) 2020 Jesse Chappell


#pragma once

#include "JuceHeader.h"

class SonoUtility
{
public:
    // should put this in some utility class
    static String durationToString(double pos, bool withColon=false, bool fractional=false)
    {
        int hours = (int) (pos/3600.0);
        int minutes = (int) (pos/60.0) % 60;
        float secs =  fmodf(pos, 60.0);
        if (hours > 0) {
            if (withColon) {
                return String::formatted("%d:%02d:%02d", hours, minutes, (int) secs);
            } else {
                return String::formatted("%dh%dm%ds", hours, minutes, (int) secs);
            }
        }
        else if (minutes > 0 || withColon) {
            if (withColon) {
                return String::formatted("%02d:%02d", minutes, (int) (secs));
            } else {
                return String::formatted("%dm%ds", minutes, (int) (secs));
            }
        }
        else if (!fractional || secs > 3.0f){
            return String::formatted("%ds", (int) (secs));
        }
        else {
            return String::formatted("%.1fs", secs);
        }
    }

    static void parseHostPort(const String& input, String& host, int& port, int defaultPort)
    {
        String trimmed = input.trim();
        if (trimmed.startsWith("[") && trimmed.contains("]")) {
            // IPv6 with brackets: [addr]:port or [addr]
            host = trimmed.fromFirstOccurrenceOf("[", false, false).upToFirstOccurrenceOf("]", false, false);
            String portPart = trimmed.fromFirstOccurrenceOf("]", false, false);
            if (portPart.startsWith(":")) {
                int p = portPart.substring(1).getIntValue();
                if (p > 0) port = p;
                else port = defaultPort;
            } else {
                port = defaultPort;
            }
        } else {
            // Check if there's exactly one colon, or multiple colons (IPv6 without brackets)
            int lastColon = trimmed.lastIndexOf(":");
            int firstColon = trimmed.indexOf(":");
            
            if (lastColon != -1 && lastColon == firstColon) {
                // Exactly one colon: host:port
                host = trimmed.substring(0, lastColon);
                int p = trimmed.substring(lastColon + 1).getIntValue();
                if (p > 0) port = p;
                else port = defaultPort;
            } else if (lastColon != -1 && trimmed.substring(lastColon + 1).containsOnly("0123456789") && trimmed.substring(lastColon + 1).isNotEmpty()) {
                // Multiple colons, but last part is numeric: assume addr:port
                host = trimmed.substring(0, lastColon);
                int p = trimmed.substring(lastColon + 1).getIntValue();
                if (p > 0) port = p;
                else port = defaultPort;
            } else {
                // No colon or multiple colons without clear port: assume address only
                host = trimmed;
                port = defaultPort;
            }
        }
    }
};
