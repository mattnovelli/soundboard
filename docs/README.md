# Soundboard config page

This folder holds the hosted configuration page (`index.html`) for the
Soundboard Pebble app. It lets you add sounds (name + emoji + audio), which are
resampled to 8 kHz 8-bit mono PCM in the browser and stored on your phone.

## Publish with GitHub Pages

1. Push this repository to GitHub.
2. In the repository settings, open **Pages**.
3. Set the source to branch `main`, folder `/docs`, and save.
4. Wait for the deploy. Your page URL will look like:
   `https://<your-username>.github.io/soundboard/`

## Wire it up

Set the published URL in [`src/pkjs/index.js`](../src/pkjs/index.js):

```js
var CONFIG_PAGE_URL = "https://<your-username>.github.io/soundboard/";
```

Then rebuild and reinstall the app.

## How it works

- PebbleKit JS opens this page via `Pebble.openURL`, passing the current sound
  list (metadata only) in the `settings` query parameter.
- The page decodes uploaded/recorded audio with the Web Audio API, resamples it
  to 8 kHz signed 8-bit mono PCM, and lets you trim/preview.
- On **Save**, the page returns the sound list plus the PCM for any new/changed
  sounds via `pebblejs://close#<json>`; PebbleKit JS stores it and streams it to
  the watch on demand.
- To keep the response small, only new/changed audio is returned. Add and save a
  few sounds at a time.
