// The web distributions' viewer-request Function (ops/web.yaml), run in node
// against the rules the template's comment states. The code is read out of the
// template itself, so what is tested is what CloudFront runs.
//
//   node --test c_base/ops/test/web_router.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const TEMPLATE = join(dirname(fileURLToPath(import.meta.url)), '..', 'web.yaml');

/** Every router block in the template, dedented. */
function routerBlocks() {
    const lines = readFileSync(TEMPLATE, 'utf8').split('\n');
    const blocks = [];
    for (let i = 0; i < lines.length; i++) {
        if (!lines[i].trim().startsWith('// ---- web router begin')) continue;
        const indent = lines[i].indexOf('//');
        const body = [];
        for (i++; !lines[i].trim().startsWith('// ---- web router end'); i++) body.push(lines[i].slice(indent));
        blocks.push(body.join('\n'));
    }
    return blocks;
}

const FOOLISH_PAGES = '404 _not-found _shell about dashboard delete-account history imessage-privacy index leaderboard privacy support tutorial';
const UTTT_PAGES = '243 404 _not-found _shell about index privacy-msg support-msg';

/** The handler as CloudFront would load it, for a distribution whose default site is `site`. */
function router(site) {
    const code = routerBlocks()[0]
        .replaceAll('${FoolishPages}', FOOLISH_PAGES)
        .replaceAll('${UtttPages}', UTTT_PAGES);
    assert.ok(!code.includes('${'), 'a substitution the test does not know about');
    return new Function(`${code}\nvar DEFAULT_SITE = '${site}';\nreturn handler;`)();
}

function req(host, uri, { cookie, query } = {}) {
    return {
        request: {
            uri,
            headers: { host: { value: host } },
            cookies: cookie ? { 'cbase-site': { value: cookie } } : {},
            querystring: query ?? {},
        },
    };
}

const foolish = router('foolish');
const uttt = router('uttt');
const key = (h, host, uri, opts) => h(req(host, uri, opts)).uri;

test('both distributions run the same router', () => {
    const blocks = routerBlocks();
    assert.equal(blocks.length, 2, 'expected a FoolishRouter and a UtttRouter block');
    assert.equal(blocks[0], blocks[1], 'FoolishRouter and UtttRouter have drifted apart');
});

test('a known page gets .html under its site prefix, from either name', () => {
    for (const host of ['foolish.cards', 'www.foolish.cards', 'FOOLISH.CARDS']) {
        assert.equal(key(foolish, host, '/dashboard'), '/foolish/dashboard.html');
        assert.equal(key(foolish, host, '/'), '/foolish/index.html');
    }
    assert.equal(key(foolish, 'foolish.cards', '/dashboard/'), '/foolish/dashboard.html');
});

test('the App Store URLs reach the static policy pages', () => {
    for (const p of ['privacy', 'imessage-privacy', 'support']) {
        assert.equal(key(foolish, 'foolish.cards', `/${p}`), `/foolish/${p}.html`);
    }
    assert.equal(key(uttt, 'uttt.live', '/privacy-msg'), '/uttt/privacy-msg.html');
    assert.equal(key(uttt, 'uttt.live', '/support-msg'), '/uttt/support-msg.html');
});

test('any other one-segment path is the shell: game ids and replay codes', () => {
    assert.equal(key(foolish, 'foolish.cards', '/abc123'), '/foolish/_shell.html');
    assert.equal(key(foolish, 'foolish.cards', '/AJG7NYNQG4RFXXGVPGD7QR2ETSOYTWMC'), '/foolish/_shell.html');
    assert.equal(key(uttt, 'uttt.live', '/AJG7NYNQG4RFXXGVPGD7QR2ETSOYTWMCREAQ'), '/uttt/_shell.html');
    // a page name as a prefix of the path is not that page
    assert.equal(key(foolish, 'foolish.cards', '/dash'), '/foolish/_shell.html');
});

test('files pass untouched, so a missing chunk stays a 404', () => {
    for (const p of ['/_next/static/chunks/0a1b.js', '/_next/static/media/bots.wasm.08p.jvewh7r_1.gz',
                     '/oracle.wasm.gz', '/dashboard.txt', '/manifest.json', '/favicon.ico']) {
        assert.equal(key(foolish, 'foolish.cards', p), `/foolish${p}`);
    }
    assert.equal(key(foolish, 'foolish.cards', '/.well-known/apple-app-site-association'),
        '/foolish/.well-known/apple-app-site-association');
    assert.equal(key(uttt, 'uttt.live', '/uttt.wasm'), '/uttt/uttt.wasm');
    assert.equal(key(uttt, 'uttt.live', '/napkin.png'), '/uttt/napkin.png');
});

test('a deeper unknown path is the 404 page, not the shell', () => {
    assert.equal(key(foolish, 'foolish.cards', '/abc/def'), '/foolish/404.html');
});

test('utils has no shell: every clean URL is its .html, and a miss is S3\'s', () => {
    assert.equal(key(foolish, 'utils.foolish.cards', '/'), '/utils/index.html');
    assert.equal(key(foolish, 'utils.foolish.cards', '/fern'), '/utils/fern.html');
    assert.equal(key(foolish, 'utils.foolish.cards', '/toys/fern'), '/utils/toys/fern.html');
});

test('uttt redirects /234 to /243 with its query, and only uttt does', () => {
    const r = uttt(req('uttt.live', '/234', { query: { seed: { value: '00ff' } } }));
    assert.equal(r.statusCode, 302);
    assert.equal(r.headers.location.value, '/243?seed=00ff');
    assert.equal(uttt(req('www.uttt.live', '/234')).headers.location.value, '/243');
    assert.equal(key(uttt, 'uttt.live', '/243'), '/uttt/243.html');
    assert.equal(key(foolish, 'foolish.cards', '/234'), '/foolish/_shell.html');
});

test('on the cloudfront.net name the cookie picks the site, else the distribution default', () => {
    const cf = 'd111111abcdef8.cloudfront.net';
    assert.equal(key(foolish, cf, '/dashboard'), '/foolish/dashboard.html');
    assert.equal(key(uttt, cf, '/243'), '/uttt/243.html');
    assert.equal(key(foolish, cf, '/fern', { cookie: 'utils' }), '/utils/fern.html');
    assert.equal(key(foolish, cf, '/243', { cookie: 'uttt' }), '/uttt/243.html');
    assert.equal(key(foolish, cf, '/dashboard', { cookie: 'nonsense' }), '/foolish/dashboard.html');
    // a real host is never overridden by the cookie
    assert.equal(key(foolish, 'foolish.cards', '/243', { cookie: 'uttt' }), '/foolish/_shell.html');
});
