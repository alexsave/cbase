// CbClient.swift - the iOS host of the c_base client (c_base/client/include/cbase/client.h, linked
// as C into the app). Everything about the protocol is the C client's: the requests and their
// bodies, the session and its refresh, which answer is a refusal. This file owns what only a host
// has: URLSession, one timer, and the randomness the C client asks for. It spells no byte offset:
// cbc_req_hdr, cbc_event and cbc_user are read through structgen's snapshots (ClientLayout.swift,
// `make -C c_base/client swift`), and the constants are cbase/client.h's own, imported by Clang.
//
// This is the HTTP half: what a sign-in, its refresh and an app's HTTP calls need. The socket half
// (CbSocket, CLIENT_DESIGN.md 3.4) lands with the next iOS client step.
import Foundation
import Security

public struct CbUser: Equatable, Sendable {
    public let userId: String
    public let name: String
    public let expiresAt: Date
}

// A call that finished without its answer: the HTTP status (0: it never arrived), and the refusal's
// why, code and detail (cbase/auth_wire.h CBW_WHY_*, CBC_WHY_LOST), or the client's own refusal of
// the call (a CBC_E_* in code, why 0).
public struct CbError: Error, Equatable, Sendable {
    public let status: Int
    public let why: Int
    public let code: Int
    public let detail: Int
    public func isAccount(_ c: Int32) -> Bool { why == Int(CBW_WHY_ACCOUNT) && code == Int(c) }
}

@MainActor
public final class CbClient {
    public let server: URL
    public var onSession: ((CbUser?) -> Void)?

    private let c: UnsafeMutablePointer<cbc>
    private let out: UnsafeMutablePointer<UInt8>
    private static let outCap = 65536
    private let urlSession: URLSession
    private var waiting: [UInt32: CheckedContinuation<Data, Error>] = [:]
    private var timer: Task<Void, Never>?
    private var closed = false

    // server: http(s)://host[:port]. session: a blob cbc_session_save wrote, to sign in as before.
    public init(server: URL, session: Data? = nil, urlSession: URLSession = .shared) {
        self.server = server
        self.urlSession = urlSession
        c = UnsafeMutablePointer<cbc>.allocate(capacity: 1)
        out = UnsafeMutablePointer<UInt8>.allocate(capacity: Self.outCap)
        var cfg = cbc_cfg()
        cbc_cfg_default(&cfg)
        cfg.seed = Self.random64()
        cbc_init(c, &cfg)
        if let session { _ = session.withUnsafeBytes { cbc_session_load(c, $0.bindMemory(to: UInt8.self).baseAddress, $0.count) } }
    }

    isolated deinit {
        timer?.cancel()
        c.deallocate()
        out.deallocate()
    }

    public func close() {
        closed = true
        timer?.cancel()
        for w in waiting.values { w.resume(throwing: CbError(status: 0, why: Int(CBC_WHY_LOST), code: 0, detail: 0)) }
        waiting.removeAll()
    }

    public var user: CbUser? {
        guard let u = try? readcbc_user(UnsafeRawPointer(cbc_user_of(c))), u.signedIn != 0 else { return nil }
        return CbUser(userId: u.userId, name: u.name, expiresAt: Date(timeIntervalSince1970: Double(u.expiresAt) / 1000))
    }

    // The session as a blob to keep (the Keychain is the app's choice), nil when signed out.
    public func sessionBlob() -> Data? {
        let n = cbc_session_save(c, out, Self.outCap)
        return n > 0 ? Data(bytes: out, count: Int(n)) : nil
    }

    public func signIn(name: String, password: String, signUp: Bool = false) async throws -> Data {
        let up = name.uppercased()
        let t = cbc_sign_in(c, signUp ? CBW_SIGNUP : CBW_SIGNIN, up, up.utf8.count, password, password.utf8.count, Self.now())
        return try await ticket(t)
    }

    public func signOut(global: Bool = false) async throws {
        _ = try await ticket(cbc_sign_out(c, global ? 1 : 0, Self.now()))
    }

    // ---- a provider's sign-in (cbase/client.h cbc_oidc_*) ---------------------------------------
    // Begins one: the C client makes the nonce and the state from this device's entropy and keeps
    // the raw nonce. The nonce returned is its SHA-256, which Apple's request carries.
    public func oidcBegin() -> (nonce: String, state: String) {
        var e = [UInt8](repeating: 0, count: Int(CBC_OIDC_ENTROPY))
        guard SecRandomCopyBytes(kSecRandomDefault, e.count, &e) == errSecSuccess,
              cbc_oidc_begin(c, e, e.count) == 0 else { fatalError("c_base: no entropy for a sign-in") }
        e.withUnsafeMutableBytes { _ = memset($0.baseAddress, 0, $0.count) }
        return (String(cString: cbc_oidc_nonce(c)), String(cString: cbc_oidc_state(c)))
    }

    // What the provider answered, to the server; the name is a new account's, as the person typed it
    // (the server's case step makes it the account's). Throws the server's refusal (CbError; CBW_ACCOUNT_E_NEEDS_NAME: call again with
    // a name, the same answer otherwise).
    public func oidcSignIn(idp: Int32, state: String, idToken: String, code: String?, name: String?, link: Bool = false) async throws {
        let k = code ?? "", n = name ?? ""
        let tk = cbc_oidc_sign_in(c, idp, link ? CBW_OIDC_LINK : 0, state, state.utf8.count, idToken, idToken.utf8.count,
                                  k, k.utf8.count, n, n.utf8.count, Self.now())
        _ = try await ticket(tk)
    }

    // ---- the pump ----------------------------------------------------------------------------------
    private func ticket(_ t: UInt32) async throws -> Data {
        guard t != 0 else { throw CbError(status: 0, why: 0, code: Int(cbc_error(c)), detail: 0) }
        return try await withCheckedThrowingContinuation { k in
            waiting[t] = k
            pump()
        }
    }

    private static func now() -> Int64 { Int64(Date().timeIntervalSince1970 * 1000) }
    private static func random64() -> UInt64 {
        var v: UInt64 = 0
        _ = withUnsafeMutableBytes(of: &v) { SecRandomCopyBytes(kSecRandomDefault, 8, $0.baseAddress!) }
        return v
    }

    // Sends every request the client has and sets the one timer.
    private func pump() {
        guard !closed else { return }
        while true {
            let n = cbc_next_request(c, out, Self.outCap, Self.now())
            if n <= 0 { break }
            send(Int(n))
        }
        timer?.cancel()
        let due = cbc_due_ms(c, Self.now())
        if due >= 0 {
            timer = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(due) * 1_000_000)
                guard let self, !Task.isCancelled else { return }
                _ = cbc_tick(self.c, Self.now())
                self.events(nil)
                self.pump()
            }
        }
    }

    private func send(_ n: Int) {
        guard let h = try? readcbc_req_hdr(UnsafeRawPointer(out)) else { return }
        var at = cbc_req_hdrSnap.cSize
        let path = String(decoding: UnsafeBufferPointer(start: out + at, count: h.pathLen), as: UTF8.self)
        at += h.pathLen
        let auth = String(decoding: UnsafeBufferPointer(start: out + at, count: h.authLen), as: UTF8.self)
        at += h.authLen
        let body = Data(bytes: out + at, count: h.bodyLen)
        precondition(at + h.bodyLen == n, "c_base request: lengths disagree")
        let id = UInt32(truncatingIfNeeded: h.id)
        let wantBody = (h.flags & Int(CBC_RQ_BODY)) != 0
        var req = URLRequest(url: URL(string: path, relativeTo: server)!)
        req.httpMethod = h.method == Int(CBC_POST) ? "POST" : "GET"
        req.cachePolicy = .reloadIgnoringLocalCacheData
        if !auth.isEmpty { req.setValue(auth, forHTTPHeaderField: "Authorization") }
        if h.bodyLen > 0 {
            req.httpBody = body
            req.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
        }
        urlSession.dataTask(with: req) { [weak self] data, resp, _ in
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            let d = data ?? Data()
            Task { @MainActor in self?.answered(id, status, d, wantBody) }
        }.resume()
    }

    private func answered(_ id: UInt32, _ status: Int, _ d: Data, _ wantBody: Bool) {
        guard !closed else { return }
        let ok = (200..<300).contains(status)
        d.withUnsafeBytes { b in
            let p = b.bindMemory(to: UInt8.self).baseAddress
            _ = cbc_on_response(c, id, Int32(status), (wantBody || !ok) ? p : nil, d.count, Self.now())
        }
        events(d)
        pump()
    }

    private func events(_ body: Data?) {
        let n = cbc_n_events(c)
        guard n > 0 else { return }
        var evs: [cbc_eventSnap] = []
        for i in 0..<n {
            if let e = try? readcbc_event(UnsafeRawPointer(cbc_event_at(c, i))) { evs.append(e) }
        }
        for e in evs {
            switch e.kind {
            case CBC_EV_DONE:
                guard let w = waiting.removeValue(forKey: UInt32(truncatingIfNeeded: e.ticket)) else { continue }
                if e.why == 0 {
                    let off = Int(e.off), len = Int(e.len)
                    w.resume(returning: body.flatMap { off + len <= $0.count ? $0.subdata(in: off..<off + len) : nil } ?? Data())
                } else {
                    w.resume(throwing: CbError(status: e.status, why: e.why, code: e.code, detail: e.detail))
                }
            case CBC_EV_SESSION:
                onSession?(user)
            default:
                break
            }
        }
    }
}
