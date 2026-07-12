/* global localStorage, Pebble */
/* Soundboard PebbleKit JS companion.
 *
 * Responsibilities:
 *  - Open the hosted configuration page and consume its response.
 *  - Persist the sound list (metadata) and each sound's PCM audio in
 *    localStorage.
 *  - Send the sound list (emoji + name) to the watch.
 *  - When the watch asks to play a sound, stream that sound's raw 8 kHz 8-bit
 *    PCM to the watch in chunks over AppMessage.
 */

'use strict';

// ---- AppMessage keys (must match src/c/main.c) ----
var KEY_PLAY_ID = 0;
var KEY_SOUND_NAMES = 1;
var KEY_SOUND_COUNT = 2;
var KEY_PCM_META = 3;
var KEY_PCM_CHUNK = 4;
var KEY_PCM_DONE = 5;
var KEY_STATUS = 6;
var KEY_VOLUME = 7;

// TODO: set this to your published GitHub Pages URL (see docs/README.md).
var CONFIG_PAGE_URL = 'https://mattnovelli.github.io/soundboard/';

// PCM chunk size in bytes. Must fit within the watch inbox (4096) with room
// for AppMessage overhead.
var CHUNK_SIZE = 2048;

// ---------------------------------------------------------------------------
// Settings storage
// ---------------------------------------------------------------------------
// 'sb_settings' -> { volume: <1-100>, sounds: [ { id, name, emoji } ] }
// 'sb_pcm_<id>' -> base64 of the raw 8 kHz 8-bit signed mono PCM.

function getSettings() {
  try {
    var s = JSON.parse(localStorage.getItem('sb_settings')) || {};
    return {
      volume: typeof s.volume === 'number' ? s.volume : 100,
      sounds: Array.isArray(s.sounds) ? s.sounds : []
    };
  } catch (e) {
    return { volume: 100, sounds: [] };
  }
}

function saveSettings(s) {
  localStorage.setItem('sb_settings', JSON.stringify({
    volume: s.volume,
    sounds: s.sounds
  }));
}

function getPcm(id) {
  return localStorage.getItem('sb_pcm_' + id) || '';
}

function setPcm(id, b64) {
  try {
    localStorage.setItem('sb_pcm_' + id, b64);
  } catch (e) {
    console.log('Failed to store PCM for ' + id + ': ' + e);
  }
}

function removePcm(id) {
  localStorage.removeItem('sb_pcm_' + id);
}

function soundDisplay(snd) {
  var emoji = snd.emoji ? (snd.emoji + ' ') : '';
  return emoji + (snd.name || 'Sound');
}

// ---------------------------------------------------------------------------
// Sending the sound list to the watch
// ---------------------------------------------------------------------------

function sendSoundList() {
  var s = getSettings();
  var names = s.sounds.map(soundDisplay).join('\n');
  var msg = {};
  msg[KEY_SOUND_NAMES] = names;
  msg[KEY_SOUND_COUNT] = s.sounds.length;
  msg[KEY_VOLUME] = s.volume;
  Pebble.sendAppMessage(msg, function () {
    console.log('Sound list sent (' + s.sounds.length + ')');
  }, function (e) {
    console.log('Failed to send sound list: ' + JSON.stringify(e));
  });
}

function sendError(text) {
  var msg = {};
  msg[KEY_STATUS] = text;
  Pebble.sendAppMessage(msg);
}

// ---------------------------------------------------------------------------
// Streaming PCM to the watch
// ---------------------------------------------------------------------------

// Base64 decode. PebbleKit JS does not provide atob(), so decode manually.
var B64_LOOKUP = (function () {
  var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  var table = {};
  for (var i = 0; i < chars.length; i++) {
    table[chars.charAt(i)] = i;
  }
  return table;
})();

function b64ToBytes(b64) {
  var clean = String(b64).replace(/[^A-Za-z0-9+/]/g, '');
  var len = clean.length;
  var rem = len & 3; // leftover chars after full groups of 4 (0, 2 or 3)
  var byteLength = (len >> 2) * 3;
  if (rem === 2) {
    byteLength += 1;
  } else if (rem === 3) {
    byteLength += 2;
  }

  var bytes = new Uint8Array(byteLength);
  var p = 0;
  var i = 0;
  for (; i + 4 <= len; i += 4) {
    var n = (B64_LOOKUP[clean.charAt(i)] << 18) |
            (B64_LOOKUP[clean.charAt(i + 1)] << 12) |
            (B64_LOOKUP[clean.charAt(i + 2)] << 6) |
            (B64_LOOKUP[clean.charAt(i + 3)]);
    bytes[p++] = (n >> 16) & 0xff;
    bytes[p++] = (n >> 8) & 0xff;
    bytes[p++] = n & 0xff;
  }
  if (rem === 2) {
    var n2 = (B64_LOOKUP[clean.charAt(i)] << 18) |
             (B64_LOOKUP[clean.charAt(i + 1)] << 12);
    bytes[p++] = (n2 >> 16) & 0xff;
  } else if (rem === 3) {
    var n3 = (B64_LOOKUP[clean.charAt(i)] << 18) |
             (B64_LOOKUP[clean.charAt(i + 1)] << 12) |
             (B64_LOOKUP[clean.charAt(i + 2)] << 6);
    bytes[p++] = (n3 >> 16) & 0xff;
    bytes[p++] = (n3 >> 8) & 0xff;
  }
  return bytes;
}

function sendChunk(bytes, offset) {
  var total = bytes.length;
  if (offset >= total) {
    var done = {};
    done[KEY_PCM_DONE] = 1;
    Pebble.sendAppMessage(done, null, function (e) {
      console.log('Failed to send DONE: ' + JSON.stringify(e));
    });
    return;
  }

  var end = Math.min(offset + CHUNK_SIZE, total);
  var slice = bytes.subarray(offset, end);
  var arr = new Array(slice.length);
  for (var i = 0; i < slice.length; i++) {
    arr[i] = slice[i];
  }

  var msg = {};
  msg[KEY_PCM_CHUNK] = arr;
  Pebble.sendAppMessage(msg, function () {
    // Pace by waiting for the previous chunk's transport ACK.
    sendChunk(bytes, end);
  }, function (e) {
    console.log('Chunk send failed @' + offset + ': ' + JSON.stringify(e));
  });
}

function streamSound(index) {
  var s = getSettings();
  if (index < 0 || index >= s.sounds.length) {
    sendError('Invalid sound');
    return;
  }
  var snd = s.sounds[index];
  var b64 = getPcm(snd.id);
  if (!b64) {
    sendError('No audio saved');
    return;
  }
  var bytes = b64ToBytes(b64);
  if (bytes.length === 0) {
    sendError('Empty sound');
    return;
  }

  var meta = {};
  meta[KEY_PCM_META] = bytes.length;
  Pebble.sendAppMessage(meta, function () {
    sendChunk(bytes, 0);
  }, function (e) {
    console.log('Failed to send PCM meta: ' + JSON.stringify(e));
  });
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  console.log('Soundboard PKJS ready');
  sendSoundList();
});

Pebble.addEventListener('appmessage', function (e) {
  var p = e.payload || {};
  if (typeof p[KEY_PLAY_ID] === 'number') {
    streamSound(p[KEY_PLAY_ID]);
  }
});

Pebble.addEventListener('showConfiguration', function () {
  var s = getSettings();
  // Pass metadata only (no PCM) to keep the URL small.
  var meta = {
    volume: s.volume,
    sounds: s.sounds.map(function (x) {
      return { id: x.id, name: x.name, emoji: x.emoji };
    })
  };
  var url = CONFIG_PAGE_URL +
    '?t=' + Date.now() +
    '&settings=' + encodeURIComponent(JSON.stringify(meta));
  console.log('Opening config page');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) {
    return;
  }

  var incoming;
  try {
    incoming = JSON.parse(decodeURIComponent(e.response));
  } catch (err) {
    console.log('Bad config response: ' + err);
    return;
  }

  var existing = getSettings();
  var newSounds = Array.isArray(incoming.sounds) ? incoming.sounds : [];

  var keepIds = {};
  var cleanSounds = [];
  for (var i = 0; i < newSounds.length; i++) {
    var snd = newSounds[i] || {};
    var id = String(snd.id || ('s' + Date.now() + '_' + i));
    keepIds[id] = true;

    // A PCM payload is only present for new/changed sounds; otherwise we keep
    // whatever is already stored for this id.
    if (typeof snd.pcm === 'string' && snd.pcm.length) {
      setPcm(id, snd.pcm);
    }

    cleanSounds.push({
      id: id,
      name: String(snd.name || 'Sound').substring(0, 24),
      emoji: String(snd.emoji || '')
    });
  }

  // Drop PCM for sounds that were removed.
  for (var j = 0; j < existing.sounds.length; j++) {
    var oldId = existing.sounds[j].id;
    if (oldId && !keepIds[oldId]) {
      removePcm(oldId);
    }
  }

  var volume = typeof incoming.volume === 'number' ? incoming.volume : existing.volume;
  saveSettings({ volume: volume, sounds: cleanSounds });
  sendSoundList();
});
