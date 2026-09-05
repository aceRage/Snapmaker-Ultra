// The phone page's service worker (P7). Served by the hub at /r/<token>/sw.js, so its scope is
// exactly one phone link and nothing wider.
//
// It exists for one job: turn a Web Push message into a notification. Three rules shape all of it,
// and none of them is optional:
//
//   1. showNotification() from the payload FIRST. Apple revokes the push permission for a site
//      that receives a push and shows nothing; Chrome substitutes its own "this site was updated
//      in the background" placeholder.
//   2. Never make a request back to the PC a prerequisite for showing it. The push arrives over
//      the phone's own internet connection from the browser's push service - the hub may be on a
//      LAN miles away, or asleep. A fetch that rejects would settle waitUntil() with no
//      notification, i.e. rule 1 broken.
//   3. notificationclick opens the page from the notification's own data, not from anything it
//      has to look up.
//
// There is deliberately no fetch handler and no offline cache here. The page is a single large
// HTML file the hub serves with Cache-Control: no-store, and pretending to work offline when the
// PC it talks to is unreachable would be a lie.

self.addEventListener('install', function (e) {
  // Take over straight away: a person who just pressed "Enable notifications" should not have to
  // close every tab before the worker that will show them is the one running.
  self.skipWaiting();
});

self.addEventListener('activate', function (e) {
  e.waitUntil(self.clients.claim());
});

function payloadOf(event) {
  // The hub sends JSON. Anything else (a probe, a push service's own keepalive) still has to end
  // in a visible notification, so fall back rather than throwing.
  var data = { title: 'UltraOne', body: 'Something happened on your printer.' };
  if (!event.data) return data;
  try {
    var j = event.data.json();
    if (j && typeof j === 'object') {
      if (typeof j.title === 'string' && j.title) data.title = j.title;
      if (typeof j.body === 'string' && j.body) data.body = j.body;
      data.url = typeof j.url === 'string' ? j.url : '';
      data.tag = typeof j.tag === 'string' ? j.tag : '';
      data.kind = typeof j.kind === 'string' ? j.kind : '';
      data.severity = typeof j.severity === 'string' ? j.severity : 'info';
    }
  } catch (err) {
    try { var t = event.data.text(); if (t) data.body = t.slice(0, 400); } catch (e2) {}
  }
  return data;
}

self.addEventListener('push', function (event) {
  var d = payloadOf(event);
  var options = {
    body: d.body,
    // Relative to the worker's own URL, i.e. /r/<token>/icon-192.png - the same icons the
    // manifest names, so a notification looks like the app it came from.
    icon: 'icon-192.png',
    badge: 'icon-192.png',
    // One notification per printer and kind: a second "paused" for the same printer replaces the
    // first instead of stacking five identical lines on the lock screen.
    tag: d.tag || 'snapmaker-orca',
    renotify: !!d.tag,
    requireInteraction: d.severity === 'error',
    data: { url: d.url || '', kind: d.kind || '' }
  };
  // waitUntil keeps the worker alive until the notification is actually shown; showNotification
  // is the only thing inside it, on purpose (rule 2).
  event.waitUntil(self.registration.showNotification(d.title, options));
});

self.addEventListener('notificationclick', function (event) {
  event.notification.close();
  var target = (event.notification.data && event.notification.data.url) || '';
  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then(function (list) {
      // A window of ours is already open somewhere: bring that one forward rather than opening a
      // second copy of the same page.
      for (var i = 0; i < list.length; i++) {
        var c = list[i];
        if (c.url.indexOf(self.registration.scope) === 0 && 'focus' in c) return c.focus();
      }
      // Otherwise open the scope itself - './' resolves to /r/<token>/ - and fall back to the
      // link the hub put in the payload when the scope is not openable.
      if (self.clients.openWindow) return self.clients.openWindow(target || self.registration.scope);
      return undefined;
    })
  );
});

// Chrome and Firefox rotate a subscription behind our back and fire this; Safari on iOS never
// does, which is why the page also re-posts getSubscription() on every launch. Both paths end in
// the same POST, so a rotation is invisible to the person.
self.addEventListener('pushsubscriptionchange', function (event) {
  event.waitUntil(
    (function () {
      var old = event.oldSubscription || null;
      var opts = (event.oldSubscription && event.oldSubscription.options) || null;
      var sub = event.newSubscription ? Promise.resolve(event.newSubscription)
                                      : (opts ? self.registration.pushManager.subscribe(opts) : Promise.resolve(null));
      return sub.then(function (s) {
        if (!s) return undefined;
        return fetch('push/subscription', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(s.toJSON ? s.toJSON() : s)
        }).catch(function () {});
      }).catch(function () {});
    })()
  );
});
