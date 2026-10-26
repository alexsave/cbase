// SignInWithApple.tsx - the Sign in with Apple button for a page on c_base: Apple's JS SDK
// (appleid.auth.js) in popup mode, and the answer handed to the C client (CbClient.oidcBegin and
// oidcSignIn, cbase/client.h cbc_oidc_*). This file holds the button and the one call to Apple; it
// decides nothing about auth: the nonce and the state are made and kept in the module, the server
// verifies Apple's token, and the session arrives through the client's own onSession.
//
//   <SignInWithApple client={client} clientId="com.example.web" redirectURI="https://app.example/auth/apple"
//                    askName={(suggested) => prompt('Pick a name', suggested) ?? ''} onSession={...} />
//
// A NEW ACCOUNT. Apple's first sign-in of a person answers their name (and only then); the server
// answers CBW_ACCOUNT_E_NEEDS_NAME for an identity no account has, so the button asks the page for
// a name (askName, given Apple's name as the suggestion) and sends the same answer again with it. A
// name already taken is asked again, with the server's refusal.
//
// THE LOOK is Apple's Human Interface Guidelines' for the button: the Apple logo and "Sign in with
// Apple", black (or white, or white with an outline), 44 px tall by default, corner radius 6, the
// system font. Apple also lets its SDK draw the button; drawing it here keeps the page's own React
// tree whole and lets it be disabled while a sign-in is out.
import { useCallback, useEffect, useRef, useState, type CSSProperties, type ReactElement } from 'react';

import { CbError, CBW_ACCOUNT_E_EXISTS, CBW_ACCOUNT_E_NEEDS_NAME, CBW_IDP_APPLE, CBW_WHY_ACCOUNT, type CbClient, type CbUser } from '../socket.ts';

// Apple's SDK, as much of it as this file uses (developer.apple.com, "Sign in with Apple JS").
interface AppleSignInResponse {
  authorization: { id_token: string; code?: string; state?: string };
  user?: { email?: string; name?: { firstName?: string; lastName?: string } };
}
interface AppleIDAuth {
  init(o: { clientId: string; scope: string; redirectURI: string; state: string; nonce: string; usePopup: boolean }): void;
  signIn(): Promise<AppleSignInResponse>;
}
declare global {
  interface Window {
    AppleID?: { auth: AppleIDAuth };
  }
}

export const APPLE_JS = 'https://appleid.cdn-apple.com/appleauth/static/jsapi/appleid/1/en_US/appleid.auth.js';

export type AppleButtonStyle = 'black' | 'white' | 'white-outline';

// Apple's rule for the style: black on a light background, white on a dark one. With no look given
// the button follows the page's color scheme as it changes.
function useSchemeLook(given: AppleButtonStyle | undefined): AppleButtonStyle {
  const query = '(prefers-color-scheme: dark)';
  const [dark, setDark] = useState(() => typeof window !== 'undefined' && !!window.matchMedia?.(query).matches);
  useEffect(() => {
    if (given || !window.matchMedia) return;
    const m = window.matchMedia(query);
    const on = () => setDark(m.matches);
    m.addEventListener('change', on);
    return () => m.removeEventListener('change', on);
  }, [given]);
  return given ?? (dark ? 'white' : 'black');
}

export interface SignInWithAppleProps {
  client: CbClient;
  // The web's Services ID, and the return URL registered for it (Apple asks for it in popup mode too).
  clientId: string;
  redirectURI: string;
  // A new account's name: Apple's name of the person as a suggestion ('' when Apple gave none), and
  // why it is asked again ('taken' after a name another account has). '' or a rejection gives up.
  askName: (suggested: string, again?: 'taken') => Promise<string> | string;
  onSession?: (user: CbUser) => void;
  onError?: (e: unknown) => void;
  // Add the identity to the signed-in account instead of signing in (no session changes).
  link?: boolean;
  scope?: string;
  look?: AppleButtonStyle;   // none: black on a light page, white on a dark one
  height?: number;
  label?: string;
  // Where Apple's SDK is loaded from: Apple's by default; a test serves its own.
  scriptSrc?: string;
}

// Loads a script once per page and resolves when it ran.
const loading = new Map<string, Promise<void>>();
function loadScript(src: string): Promise<void> {
  let p = loading.get(src);
  if (!p) {
    p = new Promise<void>((resolve, reject) => {
      const s = document.createElement('script');
      s.src = src;
      s.async = true;
      s.onload = () => resolve();
      s.onerror = () => {
        loading.delete(src);
        reject(new Error(`could not load ${src}`));
      };
      document.head.appendChild(s);
    });
    loading.set(src, p);
  }
  return p;
}

// Apple's popup closed by the person is not an error to report.
function closedByUser(e: unknown): boolean {
  const err = (e as { error?: string } | null)?.error;
  return err === 'popup_closed_by_user' || err === 'user_cancelled_authorize';
}

function refusedWith(e: unknown, code: number): boolean {
  return e instanceof CbError && e.why === CBW_WHY_ACCOUNT && e.code === code;
}

// The Apple logo, as Apple's button carries it (a filled glyph in the label's color).
function AppleLogo({ size, color }: { size: number; color: string }): ReactElement {
  return (
    <svg width={size} height={size} viewBox="0 0 170 170" aria-hidden="true" focusable="false" style={{ flex: 'none' }}>
      <path
        fill={color}
        d="M150.4 130.3c-2.4 5.5-5.2 10.6-8.4 15.2-4.4 6.3-8 10.6-10.8 13.1-4.3 4-8.9 6-13.9 6.1-3.6 0-7.9-1-12.9-3.1-5-2.1-9.6-3.1-13.8-3.1-4.4 0-9.1 1-14.2 3.1-5.1 2.1-9.1 3.2-12.3 3.3-4.8.2-9.5-1.9-14.2-6.3-3-2.6-6.7-7.1-11.2-13.5-4.8-6.8-8.7-14.6-11.8-23.6C23.5 112 21.9 102.6 21.9 93.5c0-10.4 2.2-19.4 6.7-26.9 3.5-6 8.2-10.7 14.1-14.2 5.9-3.5 12.2-5.3 19.1-5.4 3.8 0 8.7 1.2 14.8 3.5 6.1 2.3 10 3.4 11.7 3.4 1.3 0 5.6-1.4 12.9-4.1 6.9-2.5 12.7-3.5 17.5-3.1 12.9 1 22.6 6.1 29 15.3-11.5 7-17.2 16.8-17.1 29.4.1 9.8 3.7 18 10.6 24.5 3.1 3 6.7 5.3 10.6 6.9-.8 2.5-1.7 4.8-2.6 7.1zM119.1 7.2c0 7.7-2.8 14.9-8.4 21.5-6.7 7.9-14.9 12.4-23.7 11.7-.1-.9-.2-1.9-.2-2.9 0-7.4 3.2-15.3 8.9-21.7 2.8-3.3 6.4-6 10.8-8.2 4.4-2.1 8.5-3.3 12.4-3.5.1 1 .2 2.1.2 3.1z"
      />
    </svg>
  );
}

const LOOKS: Record<AppleButtonStyle, { bg: string; fg: string; border: string }> = {
  black: { bg: '#000000', fg: '#ffffff', border: '#000000' },
  white: { bg: '#ffffff', fg: '#000000', border: '#ffffff' },
  'white-outline': { bg: '#ffffff', fg: '#000000', border: '#000000' },
};

export function SignInWithApple(p: SignInWithAppleProps): ReactElement {
  const [busy, setBusy] = useState(false);
  const [ready, setReady] = useState(false);
  const live = useRef(true);
  const scheme = useSchemeLook(p.look);
  const src = p.scriptSrc ?? APPLE_JS;
  useEffect(() => {
    live.current = true;
    loadScript(src).then(() => live.current && setReady(true), (e) => p.onError?.(e));
    return () => {
      live.current = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [src]);

  const press = useCallback(async () => {
    const apple = window.AppleID?.auth;
    if (!apple || busy) return;
    setBusy(true);
    try {
      const { nonce, state } = p.client.oidcBegin();
      apple.init({ clientId: p.clientId, scope: p.scope ?? 'name email', redirectURI: p.redirectURI, state, nonce, usePopup: true });
      const r = await apple.signIn();
      const answer = { state: r.authorization.state ?? '', idToken: r.authorization.id_token, code: r.authorization.code, link: p.link };
      const suggested = [r.user?.name?.firstName, r.user?.name?.lastName].filter(Boolean).join(' ');
      let name: string | undefined;
      let again: 'taken' | undefined;
      for (;;) {
        try {
          await p.client.oidcSignIn(CBW_IDP_APPLE, { ...answer, name });
          break;
        } catch (e) {
          const needs = refusedWith(e, CBW_ACCOUNT_E_NEEDS_NAME), taken = name !== undefined && refusedWith(e, CBW_ACCOUNT_E_EXISTS);
          if (!needs && !taken) throw e;
          again = taken ? 'taken' : undefined;
          name = (await p.askName(suggested, again)).trim();
          if (!name) return;   // given up: the begin stays, the next press makes a new one
        }
      }
      const u = p.client.user();
      if (u && !p.link) p.onSession?.(u);
    } catch (e) {
      if (!closedByUser(e)) p.onError?.(e);
    } finally {
      if (live.current) setBusy(false);
    }
  }, [busy, p]);

  const look = LOOKS[scheme];
  const h = p.height ?? 44;
  const style: CSSProperties = {
    display: 'inline-flex',
    alignItems: 'center',
    justifyContent: 'center',
    gap: Math.round(h * 0.16),
    height: h,
    minWidth: 140,
    padding: `0 ${Math.round(h * 0.4)}px`,
    borderRadius: 6,
    border: `1px solid ${look.border}`,
    background: look.bg,
    color: look.fg,
    font: `500 ${Math.round(h * 0.43)}px -apple-system, BlinkMacSystemFont, "SF Pro Text", "Helvetica Neue", Helvetica, Arial, sans-serif`,
    letterSpacing: '-0.01em',
    cursor: ready && !busy ? 'pointer' : 'default',
    opacity: ready && !busy ? 1 : 0.6,
    WebkitFontSmoothing: 'antialiased',
  };
  return (
    <button type="button" onClick={press} disabled={!ready || busy} style={style} aria-label={p.label ?? 'Sign in with Apple'}>
      <AppleLogo size={Math.round(h * 0.42)} color={look.fg} />
      <span>{p.label ?? (p.link ? 'Link Apple ID' : 'Sign in with Apple')}</span>
    </button>
  );
}
