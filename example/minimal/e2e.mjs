// e2e.mjs - the minimal app in a browser, as a person opens it: headless Chrome, driven by cdom's
// own small DevTools client (tools/cdp.mjs, in the cdom this app builds with), opens the URL
// `make serve` printed and waits for the page to show the server's answer. A test-only dependency:
// Node 22 or later (for its WebSocket) and Google Chrome (CHROME names another binary); the app
// itself needs neither.
//
//   node e2e.mjs <cdom checkout> <url>        (check.sh runs it after its curl checks)
import { pathToFileURL } from 'node:url';

const [cdom, url] = process.argv.slice(2);
const { launch, sleep } = await import(pathToFileURL(`${cdom}/tools/cdp.mjs`).href);
const want = 'hello from cbase';
const b = await launch();
let text = '';
try {
  await b.send('Page.navigate', { url });
  // The page says "asking..." until the call's CBC_EV_DONE, then shows the answer in its place.
  for (let i = 0; i < 100 && !text.includes(want); i++) {
    await sleep(50);
    text = (await b.evaluate("document.getElementById('app')?.textContent")) ?? '';
  }
} finally {
  await b.close();
}
if (text.trim() === want) {
  console.log(`ok    minimal example: the page in Chrome shows the server's answer, "${want}"`);
} else {
  console.log(`FAIL: minimal example: the page in Chrome shows "${text.trim()}", not "${want}"`);
  process.exitCode = 1;
}
