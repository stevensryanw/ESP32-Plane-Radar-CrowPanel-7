#pragma once

#include <cctype>
#include <cstring>

namespace data::airlines {

struct Airline {
  char icao[4];
  const char* name;
};

/**
 * Operators by the three-letter ICAO code that starts their callsigns. Not
 * every airline: the ones likely to pass over a hobbyist's radar, weighted to
 * North America. Sorted by code for the lookup.
 */
constexpr Airline kAirlines[] = {
    {"AAL", "American Airlines"},
    {"AAR", "Asiana Airlines"},
    {"AAY", "Allegiant Air"},
    {"ABX", "ABX Air"},
    {"ACA", "Air Canada"},
    {"AFR", "Air France"},
    {"AIC", "Air India"},
    {"AJT", "Amerijet"},
    {"AMX", "Aeromexico"},
    {"ANA", "All Nippon Airways"},
    {"ANZ", "Air New Zealand"},
    {"ARG", "Aerolineas Argentinas"},
    {"ASA", "Alaska Airlines"},
    {"ASH", "Mesa Airlines"},
    {"ATN", "Air Transport International"},
    {"AUA", "Austrian Airlines"},
    {"AVA", "Avianca"},
    {"AWI", "Air Wisconsin"},
    {"AZU", "Azul"},
    {"BAW", "British Airways"},
    {"BEL", "Brussels Airlines"},
    {"BWA", "Caribbean Airlines"},
    {"CAL", "China Airlines"},
    {"CCA", "Air China"},
    {"CES", "China Eastern"},
    {"CFG", "Condor"},
    {"CJT", "Cargojet"},
    {"CKS", "Kalitta Air"},
    {"CMP", "Copa Airlines"},
    {"CNS", "PlaneSense"},
    {"CPA", "Cathay Pacific"},
    {"CSN", "China Southern"},
    {"DAL", "Delta Air Lines"},
    {"DLH", "Lufthansa"},
    {"EDV", "Endeavor Air"},
    {"EIN", "Aer Lingus"},
    {"EJA", "NetJets"},
    {"EJM", "Executive Jet Management"},
    {"ELY", "El Al"},
    {"ENY", "Envoy Air"},
    {"ETD", "Etihad Airways"},
    {"ETH", "Ethiopian Airlines"},
    {"EVA", "EVA Air"},
    {"EWG", "Eurowings"},
    {"EZY", "easyJet"},
    {"FDX", "FedEx"},
    {"FFT", "Frontier Airlines"},
    {"FIN", "Finnair"},
    {"FLE", "Flair Airlines"},
    {"GJS", "GoJet Airlines"},
    {"GLO", "Gol"},
    {"GTI", "Atlas Air"},
    {"HAL", "Hawaiian Airlines"},
    {"IBE", "Iberia"},
    {"ICE", "Icelandair"},
    {"ITY", "ITA Airways"},
    {"JAL", "Japan Airlines"},
    {"JBU", "JetBlue"},
    {"JIA", "PSA Airlines"},
    {"JTL", "Jet Linx"},
    {"JZA", "Jazz Aviation"},
    {"KAL", "Korean Air"},
    {"KAP", "Cape Air"},
    {"KLM", "KLM"},
    {"LAN", "LATAM"},
    {"LBQ", "Quest Diagnostics"},
    {"LOT", "LOT Polish Airlines"},
    {"LXJ", "Flexjet"},
    {"MAS", "Malaysia Airlines"},
    {"MSR", "EgyptAir"},
    {"MTN", "Mountain Air Cargo"},
    {"MXY", "Breeze Airways"},
    {"NKS", "Spirit Airlines"},
    {"PAC", "Polar Air Cargo"},
    {"PAL", "Philippine Airlines"},
    {"PDT", "Piedmont Airlines"},
    {"POE", "Porter Airlines"},
    {"QFA", "Qantas"},
    {"QTR", "Qatar Airways"},
    {"QXE", "Horizon Air"},
    {"RAM", "Royal Air Maroc"},
    {"RCH", "US Air Force"},
    {"RJA", "Royal Jordanian"},
    {"ROU", "Air Canada Rouge"},
    {"RPA", "Republic Airways"},
    {"RYR", "Ryanair"},
    {"SAS", "SAS"},
    {"SCX", "Sun Country"},
    {"SIA", "Singapore Airlines"},
    {"SKW", "SkyWest"},
    {"SVA", "Saudia"},
    {"SWA", "Southwest"},
    {"SWR", "Swiss"},
    {"TAM", "LATAM Brasil"},
    {"TAP", "TAP Air Portugal"},
    {"THA", "Thai Airways"},
    {"THY", "Turkish Airlines"},
    {"TOM", "TUI Airways"},
    {"TSC", "Air Transat"},
    {"UAE", "Emirates"},
    {"UAL", "United Airlines"},
    {"UCA", "CommuteAir"},
    {"UPS", "UPS"},
    {"VIR", "Virgin Atlantic"},
    {"VIV", "Viva Aerobus"},
    {"VJA", "Vista America"},
    {"VLG", "Vueling"},
    {"VOI", "Volaris"},
    {"VXP", "Avelo Airlines"},
    {"WGN", "Western Global"},
    {"WJA", "WestJet"},
    {"WZZ", "Wizz Air"},
    {"XOJ", "XOJET"},
};

/**
 * True for an airline-style callsign: three letters, then a digit, as in
 * "JBU1337". Registrations used as callsigns ("N247NE") are not.
 */
inline bool isFlightNumber(const char* callsign) {
  if (callsign == nullptr || strlen(callsign) < 4) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isupper(static_cast<unsigned char>(callsign[i]))) {
      return false;
    }
  }
  return isdigit(static_cast<unsigned char>(callsign[3])) != 0;
}

/**
 * The operator for a callsign such as "JBU1337", or nullptr if it is not a
 * flight number or its code is not in the table.
 */
inline const char* nameForCallsign(const char* callsign) {
  if (!isFlightNumber(callsign)) {
    return nullptr;
  }
  size_t low = 0;
  size_t high = sizeof(kAirlines) / sizeof(kAirlines[0]);
  while (low < high) {
    const size_t middle = (low + high) / 2;
    const int order = strncmp(callsign, kAirlines[middle].icao, 3);
    if (order == 0) {
      return kAirlines[middle].name;
    }
    if (order < 0) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }
  return nullptr;
}

}  // namespace data::airlines
