#pragma once

namespace crowpanel::ui {

using RadarChangedFn = void (*)();

bool init();
void showRadar();
void showStatus(const char* title, const char* detail, bool warning = false);

/** Redraw radar, list, selection, and alerts from the current model. */
void refreshData();
/** Refresh Wi-Fi/data-age/aircraft-count information. */
void refreshStatus();
/** Redraw the card after a route lookup has finished. */
void refreshRoute();
/** A phone has set the radar's location: close "Find me" and re-centre. */
void phoneLocationReceived();
/** Redraw the traffic at its current projected positions, if any has moved. */
void refreshMotion();
/** How often refreshMotion() is worth calling at the current range. */
unsigned long motionFrameMs();
/** Request a fresh ADS-B snapshot after range or radar-center changes. */
void setRadarChangedFn(RadarChangedFn callback);

}  // namespace crowpanel::ui
