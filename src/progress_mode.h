#pragma once
namespace bo2lan::diagnostics {
// The explicit online-capture switch wins even when an offline profile exists.
constexpr bool UseLocalServices(bool profileConfigured,bool onlineCapture) {
    return profileConfigured&&!onlineCapture;
}
}
