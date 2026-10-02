// PebbleKit JS component for BT Guard.
//
// Listens for the "sound phone" AppMessage sent from the watch (from
// either the disconnected-menu's "Sound phone" row, or the
// reconnected-locate screen's Select action) and plays a short tone
// through the phone's speaker via the Web Audio API. No bundled audio
// file, no network dependency.

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload['SOUND_PHONE'] !== undefined) {
    playLocateTone();
  }
});

function playLocateTone() {
  try {
    var AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
      return;
    }
    var ctx = new AudioContextClass();
    var durationSeconds = 0.6;
    var beepCount = 3;
    var gapSeconds = 0.15;

    for (var i = 0; i < beepCount; i++) {
      var startAt = ctx.currentTime + i * (durationSeconds + gapSeconds);
      var oscillator = ctx.createOscillator();
      var gain = ctx.createGain();

      oscillator.type = 'sine';
      oscillator.frequency.value = 1200;
      gain.gain.setValueAtTime(0.9, startAt);
      gain.gain.exponentialRampToValueAtTime(0.001, startAt + durationSeconds);

      oscillator.connect(gain);
      gain.connect(ctx.destination);

      oscillator.start(startAt);
      oscillator.stop(startAt + durationSeconds);
    }
  } catch (err) {
    console.log('BT Guard: failed to play locate tone - ' + err);
  }
}

Pebble.addEventListener('ready', function () {
  console.log('BT Guard JS ready');
});
