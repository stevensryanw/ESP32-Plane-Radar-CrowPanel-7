#pragma once

namespace crowpanel::phone_location {

// The page a phone gets from the radar's "Find me" address. It is opened as
//     https://<the radar>/?k=<one-time key>
// and sends the phone's position back to the radar with that key. It makes no
// other request and stores nothing.
constexpr char kPage[] = R"page(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Plane Radar location</title>
<style>
  :root {
    --screen: #050b18;
    --panel: #0b172a;
    --edge: #213a55;
    --text: #f1f6fa;
    --muted: #a9c0d0;
    --accent: #29d3c2;
    --warning: #f0a52b;
  }
  * { box-sizing: border-box; }
  html { -webkit-text-size-adjust: 100%; }
  body {
    margin: 0;
    min-height: 100vh;
    min-height: 100dvh;
    display: flex;
    align-items: center;
    justify-content: center;
    padding: 24px 16px;
    background: var(--screen);
    color: var(--text);
    font: 17px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  }
  main { width: 100%; max-width: 26rem; }
  h1 { margin: 0 0 12px; font-size: 26px; line-height: 1.2; font-weight: 650; }
  p { margin: 0 0 16px; color: var(--muted); }
  button {
    width: 100%;
    min-height: 56px;
    margin: 8px 0 16px;
    border: 0;
    border-radius: 10px;
    background: var(--accent);
    color: #04201d;
    font: inherit;
    font-weight: 650;
    cursor: pointer;
  }
  button:disabled { opacity: 0.55; cursor: default; }
  button:focus-visible { outline: 3px solid var(--text); outline-offset: 3px; }
  #status {
    min-height: 3em;
    margin: 0 0 16px;
    padding: 12px 14px;
    border: 1px solid var(--edge);
    border-radius: 10px;
    background: var(--panel);
    color: var(--text);
  }
  #status:empty { display: none; }
  #status.problem { border-color: var(--warning); }
  .small { font-size: 14px; margin: 0; }
</style>
</head>
<body>
<main>
  <h1 id="heading">Set the radar's location from this phone</h1>
  <p id="lead">
    Your position goes straight to the radar over your Wi-Fi, and nowhere else.
  </p>
  <button id="send" type="button">Send my location</button>
  <p id="status" role="status" aria-live="polite"></p>
  <p class="small" id="note">
    This page comes from the radar itself and stores nothing.
  </p>
</main>
<script>
(function () {
  'use strict';
  var key = new URLSearchParams(window.location.search).get('k') || '';
  var heading = document.getElementById('heading');
  var lead = document.getElementById('lead');
  var button = document.getElementById('send');
  var status = document.getElementById('status');
  var note = document.getElementById('note');

  function say(text, problem) {
    status.textContent = text;
    status.className = problem ? 'problem' : '';
  }

  function failed(text) {
    say(text, true);
    button.disabled = false;
  }

  if (!/^[0-9a-f]{16}$/.test(key)) {
    heading.textContent = 'Scan the code on the radar';
    lead.textContent = 'This page is opened by the QR code on the radar\'s ' +
                       'Find me screen (Settings, Change, Find me).';
    button.hidden = true;
    note.hidden = true;
    return;
  }

  if (!('geolocation' in navigator)) {
    say('This browser cannot share a position. Try the phone\'s own browser.', true);
    button.disabled = true;
    return;
  }

  function send(where) {
    say('Got it, to within about ' + Math.round(where.accuracy) +
        ' m. Sending it to the radar.');
    fetch('/set?lat=' + where.latitude.toFixed(6) +
          '&lon=' + where.longitude.toFixed(6) + '&k=' + key,
          { method: 'POST', cache: 'no-store' })
      .then(function (reply) {
        return reply.text().then(function (text) {
          if (!reply.ok) {
            failed(text || 'The radar refused the position. Scan the code again.');
            return;
          }
          heading.textContent = 'Location saved';
          lead.textContent = 'The radar is now centred on where you are. ' +
                             'You can close this page.';
          button.hidden = true;
          note.hidden = true;
          say('');
        });
      })
      .catch(function () {
        failed('The radar did not answer. Check that this phone is on the ' +
               'same Wi-Fi as the radar, then try again.');
      });
  }

  button.addEventListener('click', function () {
    button.disabled = true;
    say('Asking the phone where it is. Allow it if you are asked.');
    navigator.geolocation.getCurrentPosition(function (position) {
      send(position.coords);
    }, function (error) {
      if (error.code === error.PERMISSION_DENIED) {
        failed('Location is blocked for this page. Allow it for this site in ' +
               'the browser\'s settings (on iPhone: Settings, Privacy & Security, ' +
               'Location Services, Safari Websites), then tap the button again.');
      } else if (error.code === error.TIMEOUT) {
        failed('The phone took too long to find itself. Try again, nearer a ' +
               'window if you are indoors.');
      } else {
        failed('The phone could not work out where it is. Check that Location ' +
               'Services is on, then try again.');
      }
    }, { enableHighAccuracy: true, timeout: 20000, maximumAge: 0 });
  });
})();
</script>
</body>
</html>
)page";

}  // namespace crowpanel::phone_location
