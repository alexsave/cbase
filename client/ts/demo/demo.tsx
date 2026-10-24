// demo.tsx - the Sign in with Apple button on a page of its own, against a real `cbase serve` and
// either Apple or the fake Apple (c_base/core/test/fake_apple.c). serve.mjs beside it starts all
// three and writes the page's settings into config.js; `make -C c_base/client demo` runs it.
//
// With ?auto=1 the page presses the button itself and takes Apple's suggested name, so a headless
// browser can be pointed at it and the signed-in page captured.
import { StrictMode, useEffect, useRef, useState, type ReactElement } from 'react';
import { createRoot } from 'react-dom/client';

import { CbClient, type CbUser } from '../socket.ts';
import { SignInWithApple } from '../react/SignInWithApple.tsx';

interface DemoConfig {
  server: string;      // the cbase serve the page talks to
  wasm: string;        // cbase_client_oidc.wasm
  clientId: string;    // the web's Services ID
  redirectURI: string;
  appleJs: string;     // Apple's SDK, or the fake's
  fake: boolean;
}
declare global {
  interface Window {
    CBASE_DEMO: DemoConfig;
  }
}

const cfg = window.CBASE_DEMO;
const auto = new URLSearchParams(location.search).get('auto') === '1';

function NamePrompt(p: { suggested: string; again?: 'taken'; onDone: (name: string) => void }): ReactElement {
  const [v, setV] = useState(p.suggested.split(' ')[0] ?? '');
  useEffect(() => {
    if (auto) p.onDone(v);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  return (
    <form
      className="card"
      onSubmit={(e) => {
        e.preventDefault();
        p.onDone(v);
      }}
    >
      <h2>Pick a name</h2>
      <p className="muted">{p.again ? 'That name is taken. Try another.' : 'Your account is new. Choose the name people will see.'}</p>
      <input autoFocus value={v} onChange={(e) => setV(e.target.value)} maxLength={32} />
      <div className="row">
        <button type="submit" className="plain">Continue</button>
        <button type="button" className="ghost" onClick={() => p.onDone('')}>Cancel</button>
      </div>
    </form>
  );
}

function App(): ReactElement {
  const [client, setClient] = useState<CbClient | null>(null);
  const [user, setUser] = useState<CbUser | null>(null);
  const [error, setError] = useState('');
  const [ask, setAsk] = useState<{ suggested: string; again?: 'taken'; resolve: (s: string) => void } | null>(null);
  const pressed = useRef(false);

  useEffect(() => {
    let c: CbClient | null = null;
    CbClient.open({ wasm: cfg.wasm, server: cfg.server, sessionKey: 'cbase.demo.session', onSession: setUser }).then(
      (x) => {
        c = x;
        setClient(x);
        setUser(x.user());
      },
      (e) => setError(String(e)),
    );
    return () => c?.close();
  }, []);

  // ?auto=1: press the button once it can be pressed.
  useEffect(() => {
    if (!auto || !client || user || pressed.current) return;
    const t = setInterval(() => {
      const b = document.querySelector<HTMLButtonElement>('button[aria-label="Sign in with Apple"]');
      if (b && !b.disabled) {
        pressed.current = true;
        clearInterval(t);
        b.click();
      }
    }, 50);
    return () => clearInterval(t);
  }, [client, user]);

  return (
    <main>
      <header>
        <div className="mark">c_base</div>
        <div className="muted small">{cfg.fake ? 'against the fake Apple (no Apple ID involved)' : 'against Apple'}</div>
      </header>
      {user ? (
        <section className="card">
          <h2>Signed in</h2>
          <dl>
            <dt>Name</dt>
            <dd>{user.name}</dd>
            <dt>User id</dt>
            <dd className="mono">{user.userId}</dd>
            <dt>Session ends</dt>
            <dd>{new Date(user.expiresAt).toLocaleTimeString()}</dd>
          </dl>
          <button className="ghost" onClick={() => client?.signOut().catch(() => {})}>Sign out</button>
        </section>
      ) : ask ? (
        <NamePrompt
          suggested={ask.suggested}
          again={ask.again}
          onDone={(n) => {
            setAsk(null);
            ask.resolve(n);
          }}
        />
      ) : (
        <section className="card">
          <h2>Welcome</h2>
          <p className="muted">One account for every app on this server.</p>
          {client ? (
            <SignInWithApple
              client={client}
              clientId={cfg.clientId}
              redirectURI={cfg.redirectURI}
              scriptSrc={cfg.appleJs}
              askName={(suggested, again) => new Promise<string>((resolve) => setAsk({ suggested, again, resolve }))}
              onSession={setUser}
              onError={(e) => setError(e instanceof Error ? e.message : String(e))}
            />
          ) : (
            <p className="muted">Loading…</p>
          )}
        </section>
      )}
      {error ? <p className="error">{error}</p> : null}
    </main>
  );
}

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
